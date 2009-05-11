/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla code.
 *
 * The Initial Developer of the Original Code is the Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Robert O'Callahan <robert@ocallahan.org>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef nsMediaCache_h_
#define nsMediaCache_h_

#include "nsTArray.h"
#include "nsAutoLock.h"

/**
 * Media applications want fast, "on demand" random access to media data,
 * for pausing, seeking, etc. But we are primarily interested
 * in transporting media data using HTTP over the Internet, which has
 * high latency to open a connection, requires a new connection for every
 * seek, may not even support seeking on some connections (especially
 * live streams), and uses a push model --- data comes from the server
 * and you don't have much control over the rate. Also, transferring data
 * over the Internet can be slow and/or unpredictable, so we want to read
 * ahead to buffer and cache as much data as possible.
 * 
 * The job of the media cache is to resolve this impedance mismatch.
 * The media cache reads data from Necko channels into file-backed storage,
 * and offers a random-access file-like API to the stream data
 * (nsMediaCacheStream). Along the way it solves several problems:
 * -- The cache intelligently reads ahead to prefetch data that may be
 * needed in the future
 * -- The size of the cache is bounded so that we don't fill up
 * storage with read-ahead data
 * -- Cache replacement is managed globally so that the most valuable
 * data (across all streams) is retained
 * -- The cache can suspend Necko channels temporarily when their data is
 * not wanted (yet)
 * -- The cache translates file-like seek requests to HTTP seeks,
 * including optimizations like not triggering a new seek if it would
 * be faster to just keep reading until we reach the seek point. The
 * "seek to EOF" idiom to determine file size is also handled efficiently
 * (seeking to EOF and then seeking back to the previous offset does not
 * trigger any Necko activity)
 * -- The cache also handles the case where the server does not support
 * seeking
 * -- Necko can only send data to the main thread, but nsMediaCacheStream
 * can distribute data to any thread
 * -- The cache exposes APIs so clients can detect what data is
 * currently held
 * 
 * Note that although HTTP is the most important transport and we only
 * support transport-level seeking via HTTP byte-ranges, the media cache
 * works with any kind of Necko channels and provides random access to
 * cached data even for, e.g., FTP streams.
 * 
 * The media cache is not persistent. It does not currently allow
 * data from one load to be used by other loads, either within the same
 * browser session or across browser sessions. The media cache file
 * is marked "delete on close" so it will automatically disappear in the
 * event of a browser crash or shutdown.
 * 
 * The media cache is block-based. Streams are divided into blocks of a
 * fixed size (currently 4K) and we cache blocks. A single cache contains
 * blocks for all streams.
 * 
 * The cache size is controlled by the media.cache_size preference
 * (which is in KB). The default size is 50MB.
 * 
 * The replacement policy predicts a "time of next use" for each block
 * in the cache. When we need to free a block, the block with the latest
 * "time of next use" will be evicted. Blocks are divided into
 * different classes, each class having its own predictor:
 * FREE_BLOCK: these blocks are effectively infinitely far in the future;
 * a free block will always be chosen for replacement before other classes
 * of blocks.
 * METADATA_BLOCK: these are blocks that contain data that has been read
 * by the decoder in "metadata mode", e.g. while the decoder is searching
 * the stream during a seek operation. These blocks are managed with an
 * LRU policy; the "time of next use" is predicted to be as far in the
 * future as the last use was in the past.
 * PLAYED_BLOCK: these are blocks that have not been read in "metadata
 * mode", and contain data behind the current decoder read point. (They
 * may not actually have been read by the decoder, if the decoder seeked
 * forward.) These blocks are managed with an LRU policy except that we add
 * REPLAY_DELAY seconds of penalty to their predicted "time of next use",
 * to reflect the uncertainty about whether replay will actually happen
 * or not.
 * READAHEAD_BLOCK: these are blocks that have not been read in
 * "metadata mode" and that are entirely ahead of the current decoder
 * read point. (They may actually have been read by the decoder in the
 * past if the decoder has since seeked backward.) We predict the
 * time of next use for these blocks by assuming steady playback and
 * dividing the number of bytes between the block and the current decoder
 * read point by the decoder's estimate of its playback rate in bytes
 * per second. This ensures that the blocks farthest ahead are considered
 * least valuable.
 * For efficient prediction of the "latest time of next use", we maintain
 * linked lists of blocks in each class, ordering blocks by time of
 * next use. READAHEAD_BLOCKS have one linked list per stream, since their
 * time of next use depends on stream parameters, but the other lists
 * are global.
 * 
 * A block containing a current decoder read point can contain data
 * both behind and ahead of the read point. It will be classified as a
 * PLAYED_BLOCK but we will give it special treatment so it is never
 * evicted --- it actually contains the highest-priority readahead data
 * as well as played data.
 * 
 * "Time of next use" estimates are also used for flow control. When
 * reading ahead we can predict the time of next use for the data that
 * will be read. If the predicted time of next use is later then the
 * prediction for all currently cached blocks, and the cache is full, then
 * we should suspend reading from the Necko channel.
 * 
 * Unfortunately suspending the Necko channel can't immediately stop the
 * flow of data from the server. First our desire to suspend has to be
 * transmitted to the server (in practice, Necko stops reading from the
 * socket, which causes the kernel to shrink its advertised TCP receive
 * window size to zero). Then the server can stop sending the data, but
 * we will receive data roughly corresponding to the product of the link
 * bandwidth multiplied by the round-trip latency. We deal with this by
 * letting the cache overflow temporarily and then trimming it back by
 * moving overflowing blocks back into the body of the cache, replacing
 * less valuable blocks as they become available. We try to avoid simply
 * discarding overflowing readahead data.
 * 
 * All changes to the actual contents of the cache happen on the main
 * thread, since that's where Necko's notifications happen.
 *
 * The media cache maintains at most one Necko channel for each stream.
 * (In the future it might be advantageous to relax this, e.g. so that a
 * seek to near the end of the file can happen without disturbing
 * the loading of data from the beginning of the file.) The Necko channel
 * is managed through nsMediaChannelStream; nsMediaCache does not
 * depend on Necko directly.
 * 
 * Every time something changes that might affect whether we want to
 * read from a Necko channel, or whether we want to seek on the Necko
 * channel --- such as data arriving or data being consumed by the
 * decoder --- we asynchronously trigger nsMediaCache::Update on the main
 * thread. That method implements most cache policy. It evaluates for
 * each stream whether we want to suspend or resume the stream and what
 * offset we should seek to, if any. It is also responsible for trimming
 * back the cache size to its desired limit by moving overflowing blocks
 * into the main part of the cache.
 * 
 * Streams can be opened in non-seekable mode. In non-seekable mode,
 * the cache will only call nsMediaChannelStream::CacheClientSeek with
 * a 0 offset. The cache tries hard not to discard readahead data
 * for non-seekable streams, since that could trigger a potentially
 * disastrous re-read of the entire stream. It's up to cache clients
 * to try to avoid requesting seeks on such streams.
 * 
 * nsMediaCache has a single internal monitor for all synchronization.
 * This is treated as the lowest level monitor in the media code. So,
 * we must not acquire any nsMediaDecoder locks or nsMediaStream locks
 * while holding the nsMediaCache lock. But it's OK to hold those locks
 * and then get the nsMediaCache lock.
 */
class nsMediaCache;
// defined in nsMediaStream.h
class nsMediaChannelStream;

/**
 * If the cache fails to initialize then Init will fail, so nonstatic
 * methods of this class can assume gMediaCache is non-null.
 * 
 * This class can be directly embedded as a value.
 */
class nsMediaCacheStream {
public:
  enum {
    // This needs to be a power of two
    BLOCK_SIZE = 4096
  };
  enum ReadMode {
    MODE_METADATA,
    MODE_PLAYBACK
  };

  // aClient provides the underlying transport that cache will use to read
  // data for this stream.
  nsMediaCacheStream(nsMediaChannelStream* aClient)
    : mClient(aClient), mChannelOffset(0),
      mStreamOffset(0), mStreamLength(-1), mPlaybackBytesPerSecond(10000),
      mPinCount(0), mCurrentMode(MODE_PLAYBACK), mClosed(PR_FALSE),
      mIsSeekable(PR_FALSE), mCacheSuspended(PR_FALSE),
      mMetadataInPartialBlockBuffer(PR_FALSE) {}
  ~nsMediaCacheStream();

  // Set up this stream with the cache. Can fail on OOM. Must be called
  // before other methods on this object; no other methods may be called
  // if this fails.
  nsresult Init();

  // These are called on the main thread.
  // Tell us whether the stream is seekable or not. Non-seekable streams
  // will always pass 0 for aOffset to CacheClientSeek. This should only
  // be called while the stream is at channel offset 0. Seekability can
  // change during the lifetime of the nsMediaCacheStream --- every time
  // we do an HTTP load the seekability may be different (and sometimes
  // is, in practice, due to the effects of caching proxies).
  void SetSeekable(PRBool aIsSeekable);
  // This must be called (and return) before the nsMediaChannelStream
  // used to create this nsMediaCacheStream is deleted.
  void Close();
  // This returns true when the stream has been closed
  PRBool IsClosed() const { return mClosed; }

  // These callbacks are called on the main thread by the client
  // when data has been received via the channel.
  // Tells the cache what the server said the data length is going to be.
  // The actual data length may be greater (we receive more data than
  // specified) or smaller (the stream ends before we reach the given
  // length), because servers can lie. The server's reported data length
  // *and* the actual data length can even vary over time because a
  // misbehaving server may feed us a different stream after each seek
  // operation. So this is really just a hint. The cache may however
  // stop reading (suspend the channel) when it thinks we've read all the
  // data available based on an incorrect reported length. Seeks relative
  // EOF also depend on the reported length if we haven't managed to
  // read the whole stream yet.
  void NotifyDataLength(PRInt64 aLength);
  // Notifies the cache that a load has begun. We pass the offset
  // because in some cases the offset might not be what the cache
  // requested. In particular we might unexpectedly start providing
  // data at offset 0. This need not be called if the offset is the
  // offset that the cache requested in
  // nsMediaChannelStream::CacheClientSeek.
  void NotifyDataStarted(PRInt64 aOffset);
  // Notifies the cache that data has been received. The stream already
  // knows the offset because data is received in sequence and
  // the starting offset is known via NotifyDataStarted or because
  // the cache requested the offset in
  // nsMediaChannelStream::CacheClientSeek, or because it defaulted to 0.
  void NotifyDataReceived(PRInt64 aSize, const char* aData);
  // Notifies the cache that the channel has closed with the given status.
  void NotifyDataEnded(nsresult aStatus);

  // These methods can be called on any thread.
  // Cached blocks associated with this stream will not be evicted
  // while the stream is pinned.
  void Pin();
  void Unpin();
  // See comments above for NotifyDataLength about how the length
  // can vary over time. Returns -1 if no length is known. Returns the
  // reported length if we haven't got any better information. If
  // the stream ended normally we return the length we actually got.
  // If we've successfully read data beyond the originally reported length,
  // we return the end of the data we've read.
  PRInt64 GetLength();
  // Returns the end of the bytes starting at the given offset
  // which are in cache.
  PRInt64 GetCachedDataEnd(PRInt64 aOffset);
  // XXX we may need to add GetUncachedDataEnd at some point.
  // IsDataCachedToEndOfStream returns true if all the data from
  // aOffset to the end of the stream (the server-reported end, if the
  // real end is not known) is in cache. If we know nothing about the
  // end of the stream, this returns false.
  PRBool IsDataCachedToEndOfStream(PRInt64 aOffset);
  // The mode is initially MODE_PLAYBACK.
  void SetReadMode(ReadMode aMode);
  // This is the client's estimate of the playback rate assuming
  // the media plays continuously. The cache can't guess this itself
  // because it doesn't know when the decoder was paused, buffering, etc.
  // Do not pass zero.
  void SetPlaybackRate(PRUint32 aBytesPerSecond);

  // These methods must be called on a different thread from the main
  // thread. They should always be called on the same thread for a given
  // stream.
  // This can fail when aWhence is NS_SEEK_END and no stream length
  // is known.
  nsresult Seek(PRInt32 aWhence, PRInt64 aOffset);
  PRInt64 Tell();
  // *aBytes gets the number of bytes that were actually read. This can
  // be less than aCount. If the first byte of data is not in the cache,
  // this will block until the data is available or the stream is
  // closed, otherwise it won't block.
  nsresult Read(char* aBuffer, PRUint32 aCount, PRUint32* aBytes);

private:
  friend class nsMediaCache;

  /**
   * A doubly-linked list of blocks. Each block can belong to at most
   * one list at a time. Add/Remove/Get methods are all constant time.
   * We declare this here so that a stream can contain a BlockList of its
   * read-ahead blocks. Blocks are referred to by index into the
   * nsMediaCache::mIndex array.
   */
  class BlockList {
  public:
    BlockList() : mFirstBlock(-1), mCount(0) {}
    ~BlockList() {
      NS_ASSERTION(mFirstBlock == -1 && mCount == 0,
                   "Destroying non-empty block list");
    }
    void AddFirstBlock(PRInt32 aBlock);
    void AddAfter(PRInt32 aBlock, PRInt32 aBefore);
    void RemoveBlock(PRInt32 aBlock);
    // Returns the first block in the list, or -1 if empty
    PRInt32 GetFirstBlock() const { return mFirstBlock; }
    // Returns the last block in the list, or -1 if empty
    PRInt32 GetLastBlock() const;
    PRBool IsEmpty() const { return mFirstBlock < 0; }
    PRInt32 GetCount() const { return mCount; }
    // The contents of aBlockIndex1 and aBlockIndex2 have been swapped;
    // update mFirstBlock if it refers to either of these
    void NotifyBlockSwapped(PRInt32 aBlockIndex1, PRInt32 aBlockIndex2);
#ifdef DEBUG
    // Verify linked-list invariants
    void Verify();
#else
    void Verify() {}
#endif

  private:
    // The index of the first block in the list, or -1 if the list is empty.
    PRInt32 mFirstBlock;
    // The number of blocks in the list.
    PRInt32 mCount;
  };

  // Returns the end of the bytes starting at the given offset
  // which are in cache.
  // This method assumes that the cache monitor is held and can be called on
  // any thread.
  PRInt64 GetCachedDataEndInternal(PRInt64 aOffset);
  // A helper function to do the work of closing the stream. Assumes
  // that the cache monitor is held. Main thread only.
  // aMonitor is the nsAutoMonitor wrapper holding the cache monitor.
  // This is used to NotifyAll to wake up threads that might be
  // blocked on reading from this stream.
  void CloseInternal(nsAutoMonitor* aMonitor);

  // This field is main-thread-only.
  nsMediaChannelStream* mClient;

  // All other fields are all protected by the cache's monitor and
  // can be accessed by by any thread.
  // The offset where the next data from the channel will arrive
  PRInt64           mChannelOffset;
  // The offset where the reader is positioned in the stream
  PRInt64           mStreamOffset;
  // The reported or discovered length of the data, or -1 if nothing is
  // known
  PRInt64           mStreamLength;
  // For each block in the stream data, maps to the cache entry for the
  // block, or -1 if the block is not cached.
  nsTArray<PRInt32> mBlocks;
  // The list of read-ahead blocks, ordered by stream offset; the first
  // block is the earliest in the stream (so the last block will be the
  // least valuable).
  BlockList         mReadaheadBlocks;
  // The last reported estimate of the decoder's playback rate
  PRUint32          mPlaybackBytesPerSecond;
  // The number of times this stream has been Pinned without a
  // corresponding Unpin
  PRUint32          mPinCount;
  // The last reported read mode
  ReadMode          mCurrentMode;
  // Set to true when the stream has been closed either explicitly or
  // due to an internal cache error
  PRPackedBool      mClosed;
  // The last reported seekability state for the underlying channel
  PRPackedBool      mIsSeekable;
  // true if the cache has suspended our channel because the cache is
  // full and the priority of the data that would be received is lower
  // than the priority of the data already in the cache
  PRPackedBool      mCacheSuspended;
  // true if some data in mPartialBlockBuffer has been read as metadata
  PRPackedBool      mMetadataInPartialBlockBuffer;

  // Data received for the block containing mChannelOffset. Data needs
  // to wait here so we can write back a complete block. The first
  // mChannelOffset%BLOCK_SIZE bytes have been filled in with good data,
  // the rest are garbage.
  // Use PRInt64 so that the data is well-aligned.
  PRInt64           mPartialBlockBuffer[BLOCK_SIZE/sizeof(PRInt64)];
};

#endif