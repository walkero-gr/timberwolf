/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 * 
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 * 
 * The Original Code is Mozilla Communicator.
 * 
 * The Initial Developer of the Original Code is Intel Corp.
 * Portions created by Intel Corp. are
 * Copyright (C) 1999, 1999 Intel Corp.  All
 * Rights Reserved.
 * 
 * Contributor(s): Yixiong Zou <yixiong.zou@intel.com>
 *                 Carl Wong <carl.wong@intel.com>
 */

#include "nsNetDiskCache.h"
#include "nscore.h"

#include "plstr.h"
#include "prprf.h"
#include "prtypes.h"
#include "prio.h"
#include "prsystem.h" // Directory Seperator
#include "plhash.h"
#include "prclist.h"
#include "prmem.h"
#include "prlog.h"

#include "nsIComponentManager.h"
#include "nsIServiceManager.h"

#include "nsIPref.h"
#include "mcom_db.h"
#include "nsDBEnumerator.h"

#include "nsDiskCacheRecord.h"
#include "netCore.h"
#include "nsFileLocations.h"
#include "nsIFileLocator.h"
#include "nsIFile.h"
#include "nsILocalFile.h"
#include "nsIFileSpec.h" // Remove Me later
#if !defined(IS_LITTLE_ENDIAN) && !defined(IS_BIG_ENDIAN)
ERROR! Must have a byte order
#endif

#ifdef IS_LITTLE_ENDIAN
#define COPY_INT32(_a,_b)  memcpy(_a, _b, sizeof(int32))
#else
#define COPY_INT32(_a,_b)  /* swap */                   \
    do {                                                \
    ((char *)(_a))[0] = ((char *)(_b))[3];              \
    ((char *)(_a))[1] = ((char *)(_b))[2];              \
    ((char *)(_a))[2] = ((char *)(_b))[1];              \
    ((char *)(_a))[3] = ((char *)(_b))[0];              \
    } while(0)
#endif

static NS_DEFINE_CID(kPrefCID, NS_PREF_CID) ;
static NS_DEFINE_CID(kDBAccessorCID, NS_DBACCESSOR_CID) ;

static const PRUint32 DISK_CACHE_SIZE_DEFAULT = 5*1024*1024 ; // 5MB
static const char * const DISK_CACHE_SIZE_PREF  = "browser.cache.disk_cache_size";
static const char * const CACHE_DIR_PREF   = "browser.cache.directory";
static const char * const CACHE_ENABLE_PREF   = "browser.cache.disk.enable";


static int folderChanged(const char *pref, void *closure)
{
	nsresult rv;
	NS_WITH_SERVICE(nsIPref, prefs, NS_PREF_PROGID, &rv);
	if ( NS_FAILED (rv ) )
		return rv;
		
	nsCOMPtr<nsIFileSpec> folder;
	rv = prefs->GetFilePref(CACHE_DIR_PREF, getter_AddRefs( folder )  );
	
	if ( NS_FAILED( rv ) )
		return rv;
	
	// This is really wrong and should be fixed when the pref code is update
	// If someone has a system where path names are not unique they are going to
	// be in a world of hurt.
	
	nsCOMPtr<nsILocalFile> cacheFolder;
	char* path = NULL;
	rv = folder->GetNativePath(& path );
	if ( NS_FAILED ( rv ) )
		return rv;
		
	rv = NS_NewLocalFile( path, getter_AddRefs(cacheFolder));
	nsAllocator::Free( path );
	
	return ( (nsNetDiskCache*)closure )->SetDiskCacheFolder( cacheFolder );	
}

static int enableChanged(const char *pref, void *closure)
{
	nsresult rv;
	NS_WITH_SERVICE(nsIPref, prefs, NS_PREF_PROGID, &rv);
	if ( NS_FAILED (rv ) )
		return rv;
		
	PRBool enabled;
	rv = prefs->GetBoolPref(CACHE_ENABLE_PREF, &enabled   );
	if ( NS_FAILED( rv ) )
		return rv;
		
	return ( (nsNetDiskCache*)closure )->SetEnabled( enabled );	
}

class nsDiskCacheRecord ;

nsNetDiskCache::nsNetDiskCache() :
  mEnabled(PR_FALSE) ,
  mNumEntries(0) ,
  mpNextCache(0) ,
  mDiskCacheFolder(0) ,
  mStorageInUse(0) ,
  mDB(0) ,
  mDBCorrupted(PR_FALSE) 
{
  // set it to INF for now 
  mMaxEntries = (PRUint32)-1 ;

  NS_INIT_REFCNT();

}

nsNetDiskCache::~nsNetDiskCache()
{
	if ( mDB )
  	SetSizeEntry();

  NS_IF_RELEASE(mDB) ;
  
  nsresult rv;
  NS_WITH_SERVICE(nsIPref, prefs, NS_PREF_PROGID, &rv);
	if ( NS_SUCCEEDED (rv ) )
	{
		 prefs->UnregisterCallback( CACHE_DIR_PREF, folderChanged, this ); 
		 prefs->UnregisterCallback( CACHE_ENABLE_PREF, enableChanged, this ); 
	}
  // FUR
  // I think that, eventually, we also want a distinguished key in the DB which
  // means "clean cache shutdown".  You clear this flag when the db is first
  // opened and set it just before the db is closed.  If the db wasn't shutdown
  // cleanly in a prior session, i.e. because the app crashed, on startup you
  // scan all the individual files in directories and look for "orphans",
  // i.e. cache files which don't have corresponding entries in the db.  That's
  // also when storage-in-use and number of entries would be recomputed.
  //
  // We don't necessarily need all this functionality immediately, though.


  if(mDBCorrupted) {
    
    nsCOMPtr<nsISimpleEnumerator> directoryEnumerator;
    nsresult res = mDiskCacheFolder->GetDirectoryEntries( getter_AddRefs( directoryEnumerator) ) ;
    if ( NS_FAILED ( res ) )
    	return;

    nsCString trash("trash") ;
	  nsCOMPtr<nsIFile> file;
	  
	  PRBool hasMore;
    while( NS_SUCCEEDED(directoryEnumerator->HasMoreElements( &hasMore ) ) && hasMore)
    {
    	nsresult rv = directoryEnumerator->GetNext( getter_AddRefs(file) );
    	if ( NS_FAILED( rv ) )
    		return ;
    		
      char* filename;
      rv = file->GetLeafName( &filename );
      if ( NS_FAILED( rv ) )
    		return;
    	

      if( trash.CompareWithConversion( filename, PR_FALSE, 5 ) == 0)
        file->Delete( PR_TRUE );
      
      nsCRT::free(filename) ;  
    }
  }
}

NS_IMETHODIMP nsNetDiskCache::InitCacheFolder()
{

// don't initialize if no cache folder is set. 
 if(!mDiskCacheFolder) return NS_OK ;
	
	nsresult rv;
	if(!mDB) {
    mDB = new nsDBAccessor() ;
    if(!mDB)
      return NS_ERROR_OUT_OF_MEMORY ;
    else
      NS_ADDREF(mDB) ;
  }


  rv = InitDB();
  if ( NS_FAILED( rv ) )
  	rv = DBRecovery();
  if ( NS_FAILED( rv ) )
  	return rv;
  
  // create cache sub directories
  // Do this after initializing the database to avoid the situtation where on the first initialization
  // YOu create the folders and then immediately mark them for deletion.
  nsCOMPtr<nsIFile> cacheSubDir;

  for (int i=0; i < 32; i++) {
   	rv = mDiskCacheFolder->Clone( getter_AddRefs( cacheSubDir ) );
    if(NS_FAILED(rv))
      return rv ;

    char dirName[3];
    PR_snprintf (dirName, 3, "%0.2x", i);
    cacheSubDir->Append (dirName) ;
    CreateDir(cacheSubDir);
  }
  return rv;
}

NS_IMETHODIMP
nsNetDiskCache::Init(void) 
{
	nsresult rv;
	NS_WITH_SERVICE(nsIPref, prefs, NS_PREF_PROGID, &rv);
	if ( NS_FAILED (rv ) )
		return rv; 
	rv = prefs->RegisterCallback( CACHE_DIR_PREF, folderChanged, this); 
	if ( NS_FAILED( rv ) )
		return rv;
	
	rv = prefs->RegisterCallback( CACHE_ENABLE_PREF, enableChanged, this); 
	if ( NS_FAILED( rv ) )
		return rv;
	
	rv = folderChanged(CACHE_DIR_PREF , this );	
	enableChanged(CACHE_ENABLE_PREF , this );
	
	return rv;
}

NS_IMETHODIMP
nsNetDiskCache::InitDB(void)
{
  nsresult rv ; 

	rv =mDiskCacheFolder->Clone( getter_AddRefs( mDBFile ) );
  if(NS_FAILED(rv))
    return rv ;

  mDBFile->Append("cache.db") ;
	#ifdef XP_MAC
		PRBool exists;
		if ( NS_SUCCEEDED( mDBFile->Exists(&exists ) ) && !exists)
		{
			mDBFile->Create( nsIFile::NORMAL_FILE_TYPE, PR_IRUSR | PR_IWUSR );
		}
	#endif
  rv = mDB->Init(mDBFile) ;
	if ( NS_SUCCEEDED( rv ) )
	  rv = GetSizeEntry();
  return rv ;
}

//////////////////////////////////////////////////////////////////////////
// nsISupports methods

NS_IMPL_THREADSAFE_ISUPPORTS3(nsNetDiskCache,
                   nsINetDataDiskCache,
                   nsINetDataCache,
                   nsISupports) 

///////////////////////////////////////////////////////////////////////////
// nsINetDataCache Method 

NS_IMETHODIMP
nsNetDiskCache::GetDescription(PRUnichar* *aDescription) 
{
  nsAutoString description ;
  description.AssignWithConversion("Disk Cache") ;

  *aDescription = description.ToNewUnicode() ;
  if(!*aDescription)
    return NS_ERROR_OUT_OF_MEMORY ;

  return NS_OK ;
}

/* don't alloc mem for nsICachedNetData. 
 * RecordID is generated using the same scheme in nsCacheDiskData,
 * see GetCachedNetData() for detail.
 */
NS_IMETHODIMP
nsNetDiskCache::Contains(const char* key, PRUint32 length, PRBool *_retval) 
{
  *_retval = PR_FALSE ;

  NS_ASSERTION(mDB, "no db.") ;

  PRInt32 id = 0 ;
  nsresult rv = mDB->GetID(key, length, &id) ;

  if(NS_FAILED(rv)) {
    // try recovery if error 
    DBRecovery() ;
    return rv ;
  }

  void* info = 0 ;
  PRUint32 info_size = 0 ;

  rv = mDB->Get(id, &info, &info_size) ;
  if(NS_SUCCEEDED(rv) && info) 
    *_retval = PR_TRUE ;
        
  if(NS_FAILED(rv)) {
    // try recovery if error 
    DBRecovery() ;
  }

  return rv ;
}

/* regardless if it's cached or not, a copy of nsNetDiskCache would 
 * always be returned. so release it appropriately. 
 * if mem allocated, update mNumEntries also.
 * for now, the new nsCachedNetData is not written into db yet since
 * we have nothing to write.
 */
NS_IMETHODIMP
nsNetDiskCache::GetCachedNetData(const char* key, PRUint32 length, nsINetDataCacheRecord **_retval) 
{
  NS_ASSERTION(mDB, "no db.");

  nsresult rv = 0;
  if (!_retval)
    return NS_ERROR_NULL_POINTER;

  *_retval = nsnull;

  PRInt32 id = 0;
  rv = mDB->GetID(key, length, &id);

	if ( NS_SUCCEEDED ( rv )) {
	  // construct an empty record
	  nsDiskCacheRecord* newRecord = new nsDiskCacheRecord(mDB, this);
	  if (!newRecord) 
	    return NS_ERROR_OUT_OF_MEMORY;

	  rv = newRecord->Init(key, length, id);
	  if ( NS_FAILED( rv) ) {
	    delete newRecord;
	    return rv;
	  }

	  NS_ADDREF(newRecord); // addref for _retval 
	  *_retval = (nsINetDataCacheRecord*) newRecord;
	  
	  void* info = 0;
	  PRUint32 info_size = 0;

	  rv = mDB->Get(id, &info, &info_size);
	  if ( NS_SUCCEEDED( rv ) ) {
			if ( info ) {
		    // this is a previously cached record    
		    rv = newRecord->RetrieveInfo(info, info_size);
		    if ( NS_FAILED( rv ) ) {  
		      // probably a bad one
		      NS_RELEASE(newRecord);
		      *_retval = nsnull;
		      return rv;
		    }   
	 		} else {
		    // this is a new record. 
		    mNumEntries ++;
	    }
	  } 
  }
  
  if ( NS_FAILED( rv ) ) 
  	DBRecovery();
  	
  return rv;
}

NS_IMETHODIMP
nsNetDiskCache::GetCachedNetDataByID(PRInt32 RecordID, nsINetDataCacheRecord **_retval) 
{
  NS_ASSERTION(mDB, "no db.");

	nsresult rv;
  void* info = 0;
  PRUint32 info_size = 0;
	
	if (!_retval)
    return NS_ERROR_NULL_POINTER;
	*_retval = nsnull;
	 
  rv = mDB->Get(RecordID, &info, &info_size);
  if(NS_SUCCEEDED(rv) && info) {
    // construct an empty record if only found in db
    nsDiskCacheRecord* newRecord = new nsDiskCacheRecord(mDB, this);
    if(!newRecord) 
      return NS_ERROR_OUT_OF_MEMORY;
   
    rv = newRecord->RetrieveInfo(info, info_size);
    if(NS_SUCCEEDED(rv)) {
      *_retval = (nsINetDataCacheRecord*) newRecord;
      NS_ADDREF(*_retval);
    } else {
      delete newRecord; 
    }
    return rv;
  }

  NS_WARNING("Error: RecordID not in DB\n");
  DBRecovery();
  return rv;
}

NS_IMETHODIMP
nsNetDiskCache::GetEnabled(PRBool *aEnabled) 
{
  *aEnabled = mEnabled ;
  return NS_OK ;
}

NS_IMETHODIMP
nsNetDiskCache::SetEnabled(PRBool aEnabled) 
{
  mEnabled = aEnabled ;
  return NS_OK ;
}

NS_IMETHODIMP
nsNetDiskCache::GetFlags(PRUint32 *aFlags) 
{
  *aFlags = FILE_PER_URL_CACHE;
  return NS_OK ;
}

NS_IMETHODIMP
nsNetDiskCache::GetNumEntries(PRUint32 *aNumEntries) 
{
  *aNumEntries = mNumEntries ;
  return NS_OK ;
}

NS_IMETHODIMP
nsNetDiskCache::GetMaxEntries(PRUint32 *aMaxEntries)
{
  *aMaxEntries = mMaxEntries ;
  return NS_OK ;
}

NS_IMETHODIMP
nsNetDiskCache::NewCacheEntryIterator(nsISimpleEnumerator **_retval) 
{
  NS_ASSERTION(mDB, "no db.") ;

  if(!_retval)
    return NS_ERROR_NULL_POINTER ;

  *_retval = nsnull ;

  nsDBEnumerator* enumerator = new nsDBEnumerator(mDB, this) ;
  if(enumerator)
  	return  enumerator->QueryInterface( nsISimpleEnumerator::GetIID(), (void**)_retval );
  
  return NS_ERROR_OUT_OF_MEMORY ;
}

NS_IMETHODIMP
nsNetDiskCache::GetNextCache(nsINetDataCache * *aNextCache)
{
  if(!aNextCache)
    return NS_ERROR_NULL_POINTER ;

  *aNextCache = mpNextCache ;
  return NS_OK ;
}

NS_IMETHODIMP
nsNetDiskCache::SetNextCache(nsINetDataCache *aNextCache)
{
  mpNextCache = aNextCache ;
  return NS_OK ;
}

// db size can always be measured at the last minute. Since it's hard
// to know before hand. 
NS_IMETHODIMP
nsNetDiskCache::GetStorageInUse(PRUint32 *aStorageInUse)
{
  NS_ASSERTION(mDB, "no db.");

  PRUint32 total_size = mStorageInUse;

  // we need size in kB
  total_size = total_size >> 10 ;

  *aStorageInUse = total_size ;
  return NS_OK ;
}

/*
 * The whole cache dirs can be whiped clean since all the cache 
 * files are resides in seperate hashed dirs. It's safe to do so.
 */
 
NS_IMETHODIMP
nsNetDiskCache::RemoveAll(void)
{
  NS_ASSERTION(mDB, "no db.") ;
  NS_ASSERTION(mDiskCacheFolder, "no cache folder.") ;



  // Shutdown the database
  mDB->Shutdown() ;
  
  // Delete all the files in the cache directory
  // Could I just delete the cache folder????? --DJM
  nsCOMPtr<nsISimpleEnumerator> directoryEnumerator;
  nsresult res = mDiskCacheFolder->GetDirectoryEntries( getter_AddRefs( directoryEnumerator) ) ;
  if ( NS_FAILED ( res ) )
  	return res;

  nsCOMPtr<nsIFile> file;
  
  PRBool hasMore;
  while( NS_SUCCEEDED(directoryEnumerator->HasMoreElements( &hasMore ) ) && hasMore)
  {
  	nsresult rv = directoryEnumerator->GetNext( getter_AddRefs(file) );
  	if ( NS_FAILED( rv ) )
  		return rv ;
  	PRBool isDirectory;		
		if ( NS_SUCCEEDED(file->IsDirectory( &isDirectory )) && isDirectory )
    	file->Delete( PR_TRUE );
     
  }
  if ( mDBFile )
 	 mDBFile->Delete(PR_FALSE) ;
  // reinitilize
  return InitCacheFolder() ;
}

//////////////////////////////////////////////////////////////////
// nsINetDataDiskCache methods

NS_IMETHODIMP
nsNetDiskCache::GetDiskCacheFolder(nsIFile * *aDiskCacheFolder)
{
  *aDiskCacheFolder = nsnull ;
  NS_ASSERTION(mDiskCacheFolder, "no cache folder.") ;

  *aDiskCacheFolder = mDiskCacheFolder ;
  NS_ADDREF(*aDiskCacheFolder) ;
  return NS_OK ;
}

NS_IMETHODIMP
nsNetDiskCache::SetDiskCacheFolder(nsIFile * aDiskCacheFolder)
{
  if(!mDiskCacheFolder){
    mDiskCacheFolder = aDiskCacheFolder ;
    return InitCacheFolder() ;
  } 
  else {
		PRBool result;
    if( NS_SUCCEEDED( mDiskCacheFolder->Equals( aDiskCacheFolder, &result ) ) && result ) {
      mDiskCacheFolder = aDiskCacheFolder ;

      // do we need to blow away old cache before building a new one?
     	// mDiskFolder points to the old cache so you can't call RemoveALL
     	// need to refactor.
     // return RemoveAll() ;

      mDB->Shutdown() ;
      return InitCacheFolder() ;

    } else 
      return NS_OK ;
  }
}

//////////////////////////////////////////////////////////////////
// nsNetDiskCache methods

// create a directory (recursively)
NS_IMETHODIMP
nsNetDiskCache::CreateDir(nsIFile* dir_spec) 
{
  PRBool does_exist ;
  nsCOMPtr<nsIFile> p_spec ;

  dir_spec->Exists(&does_exist) ;
  if(does_exist) 
    return NS_OK ;

  nsresult rv = dir_spec->GetParent(getter_AddRefs(p_spec)) ;
  if(NS_FAILED(rv))
    return rv ;

  p_spec->Exists(&does_exist) ;
  if(!does_exist) {
    rv = CreateDir(p_spec) ;
  	if ( NS_FAILED( rv ))
  		return rv;
  }
  
  rv = dir_spec->Create( nsIFile::DIRECTORY_TYPE, PR_IRUSR | PR_IWUSR) ;  
  return rv ;
}

// We can't afford to make a *separate* pass over the whole db on every
// startup, just to figure out mNumEntries and mStorageInUse.  (This is a
// several second operation on a large db).  We'll likely need to store
// distinguished keys in the db that contain these values and update them
// incrementally, except when failure to shut down the db cleanly is detected.

NS_IMETHODIMP
nsNetDiskCache::GetSizeEntry(void)
{
  void* pInfo ;
  PRUint32 InfoSize ;

  nsresult rv = mDB->GetSizeEntry(&pInfo, &InfoSize) ;
  if(NS_FAILED(rv))
    return rv ;

  if(!pInfo && InfoSize == 0) {
    // must be a new DB
    mNumEntries = 0 ;
    mStorageInUse = 0 ;
  } 
  else {
    char * cur_ptr = NS_STATIC_CAST(char*, pInfo) ;
    
    // get mNumEntries
    COPY_INT32(&mNumEntries, cur_ptr) ;
    cur_ptr += sizeof(PRUint32) ;
    
    // get mStorageInUse
    COPY_INT32(&mStorageInUse, cur_ptr) ;
    cur_ptr += sizeof(PRUint32) ;
    
    PR_ASSERT(cur_ptr == NS_STATIC_CAST(char*, pInfo) + InfoSize);
  }

  return NS_OK ;
}

NS_IMETHODIMP
nsNetDiskCache::SetSizeEntry(void)
{
  PRUint32 InfoSize ;
  
  InfoSize = sizeof mNumEntries ;
  InfoSize += sizeof mStorageInUse ;
  
  void* pInfo = nsAllocator::Alloc(InfoSize*sizeof(char)) ;
  if(!pInfo) 
    return NS_ERROR_OUT_OF_MEMORY ;
  
  char* cur_ptr = NS_STATIC_CAST(char*, pInfo) ;

  COPY_INT32(cur_ptr, &mNumEntries) ;
  cur_ptr += sizeof(PRUint32) ;

  COPY_INT32(cur_ptr, &mStorageInUse) ;
  cur_ptr += sizeof(PRUint32) ;
  
  PR_ASSERT(cur_ptr == NS_STATIC_CAST(char*, pInfo) + InfoSize);
  
  return mDB->SetSizeEntry(pInfo, InfoSize) ;
}

// this routine will be called everytime we have a db corruption. 
// mDB will be re-initialized, mStorageInUse and mNumEntries will
// be reset.
NS_IMETHODIMP
nsNetDiskCache::DBRecovery(void)
{
  // rename all the sub cache dirs and remove them later during dtor. 
  nsresult rv = RenameCacheSubDirs() ;
  if(NS_FAILED(rv))
    return rv ;

  // remove corrupted db file, don't care if db->shutdown fails or not.
  mDB->Shutdown() ;
  

  // False since we want to delete a file rather than recursively delete a directory 
  rv = mDBFile->Delete(PR_FALSE) ;
	if (NS_FAILED ( rv ) ) return rv;
  

  // reinitilize DB 
  return InitDB() ;

}



// this routine will add string "trash" to current CacheSubDir names. 
// e.g. 00->trash00, 1f->trash1f. and update the mDBCorrupted. 

NS_IMETHODIMP
nsNetDiskCache::RenameCacheSubDirs(void) 
{
  nsCOMPtr<nsIFile> cacheSubDir;
  nsresult rv;

  for (int i=0; i < 32; i++) {
  	rv = mDiskCacheFolder->Clone( getter_AddRefs( cacheSubDir ) );
    if(NS_FAILED(rv))
      return rv ;

    char oldName[3], newName[8];
    PR_snprintf(oldName, 3, "%0.2x", i) ;
    cacheSubDir->Append(oldName) ;

    // re-name the directory
    PR_snprintf(newName, 8, "trash%0.2x", i) ;
    rv = cacheSubDir->MoveTo( mDiskCacheFolder, newName );
    if(NS_FAILED(rv))
      // TODO, error checking
      return NS_ERROR_FAILURE ;
  }

  // update mDBCorrupted 
  mDBCorrupted = PR_TRUE ;

  return NS_OK ;
}
