/* ***** BEGIN LICENSE BLOCK *****
 *
 * The contents of this file is copyrighted by Thomas and Hans-Joerg Frieden.
 * It's content is not open source and may not be redistributed, modified or adapted
 * without permission of the above-mentioned copyright holders.
 *
 * Since this code was originally developed under an AmigaOS related bounty, any derived
 * version of this file may only be used on an official AmigaOS system.
 *
 * Contributor(s):
 * 	Thomas Frieden <thomas@friedenhq.org>
 * 	Hans-Joerg Frieden <hans-joerg@friedenhq.org>
 *
 * ***** END LICENSE BLOCK ***** */


#include "nsRegisterGRE.h"
#include "nsXPCOMGlue.h"

#include "nsXPCOM.h"
#include "nsIFile.h"
#include "nsILocalFile.h"

#include "nsAppRunner.h" // for MAXPATHLEN
#include "nsStringAPI.h"
#include "nsINIParser.h"
#include "nsCOMPtr.h"

#include "prio.h"
#include "prprf.h"
#include "prenv.h"

#include <unistd.h>
#include <sys/stat.h>

// If we can't register <buildid>.conf, we try to create a unique filename
// by looping through <buildid>_<int>.conf, but if something is seriously wrong
// we stop at 1000
#define UNIQ_LOOP_LIMIT 1000

static const char kRegFileGlobal[] = "global.reginfo";
static const char kRegFileUser[] = "user.reginfo";

class AutoFDClose
{
public:
  AutoFDClose(PRFileDesc* fd = nsnull) : mFD(fd) { }
  ~AutoFDClose() { if (mFD) PR_Close(mFD); }

  PRFileDesc* operator= (PRFileDesc *fd) {
    if (mFD) PR_Close(mFD);
    mFD = fd;
    return fd;
  }

  operator PRFileDesc* () { return mFD; }
  PRFileDesc** operator &() { *this = nsnull; return &mFD; }

private:
  PRFileDesc *mFD;
};

static PRBool
MakeConfFile(const char *regfile, const nsCString &greHome,
             const GREProperty *aProperties, PRUint32 aPropertiesLen,
             const char *aGREMilestone)
{
  // If the file exists, don't create it again!
  if (access(regfile, R_OK) == 0)
    return PR_FALSE;

  PRBool ok = PR_TRUE;

  { // scope "fd" so that we can delete the file if something goes wrong
    AutoFDClose fd = PR_Open(regfile, PR_CREATE_FILE | PR_WRONLY | PR_TRUNCATE,
                             0664);
    if (!fd)
      return PR_FALSE;

    static const char kHeader[] =
      "# Registration file generated by xulrunner. Do not edit.\n\n"
      "[%s]\n"
      "GRE_PATH=%s\n";

    if (PR_fprintf(fd, kHeader, aGREMilestone, greHome.get()) <= 0)
      ok = PR_FALSE;

    for (PRUint32 i = 0; i < aPropertiesLen; ++i) {
      if (PR_fprintf(fd, "%s=%s\n",
                     aProperties[i].property, aProperties[i].value) <= 0)
        ok = PR_FALSE;
    }
  }

  if (!ok)
    PR_Delete(regfile);

  return ok;
}


PRBool
RegisterXULRunner(PRBool aRegisterGlobally, nsIFile* aLocation,
                  const GREProperty *aProperties, PRUint32 aPropertiesLen,
                  const char *aGREMilestone)
{
  // Register ourself in /etc/gre.d or ~/.gre.d/ and record what key we created
  // for future unregistration.

  nsresult rv;

  char root[MAXPATHLEN] = "S:gre.d";

  if (!aRegisterGlobally) {
    char *home = PR_GetEnv("HOME");
    if (!home || !*home)
      return PR_FALSE;

    PR_snprintf(root, MAXPATHLEN, "%s/.gre.d", home);
  }

  nsCString greHome;
  rv = aLocation->GetNativePath(greHome);
  if (NS_FAILED(rv))
    return rv;

  nsCOMPtr<nsIFile> savedInfoFile;
  aLocation->Clone(getter_AddRefs(savedInfoFile));
  nsCOMPtr<nsILocalFile> localSaved(do_QueryInterface(savedInfoFile));
  if (!localSaved)
    return PR_FALSE;

  const char *infoname = aRegisterGlobally ? kRegFileGlobal : kRegFileUser;
  localSaved->AppendNative(nsDependentCString(infoname));

  AutoFDClose fd;
  rv = localSaved->OpenNSPRFileDesc(PR_CREATE_FILE | PR_RDWR, 0664, &fd);
  // XXX report error?
  if (NS_FAILED(rv))
    return PR_FALSE;

  char keyName[MAXPATHLEN];

  PRInt32 r = PR_Read(fd, keyName, MAXPATHLEN);
  if (r < 0)
    return PR_FALSE;

  char regfile[MAXPATHLEN];

  if (r > 0) {
    keyName[r] = '\0';

    PR_snprintf(regfile, MAXPATHLEN, "%s/%s.conf", root, keyName);

    // There was already a .reginfo file, let's see if we are already
    // registered.
    if (access(regfile, R_OK) == 0) {
      fprintf(stderr, "Warning: Configuration file '%s' already exists.\n"
                      "No action was performed.\n", regfile);
      return PR_FALSE;
    }

    rv = localSaved->OpenNSPRFileDesc(PR_CREATE_FILE | PR_WRONLY | PR_TRUNCATE, 0664, &fd);
    if (NS_FAILED(rv))
      return PR_FALSE;
  }

  if (access(root, R_OK | X_OK) &&
      mkdir(root, 0775)) {
    fprintf(stderr, "Error: could not create '%s'.\n",
            root);
    return PR_FALSE;
  }

  PR_snprintf(regfile, MAXPATHLEN, "%s/%s.conf", root, aGREMilestone);
  if (MakeConfFile(regfile, greHome, aProperties, aPropertiesLen,
                   aGREMilestone)) {
    PR_fprintf(fd, "%s", aGREMilestone);
    return PR_TRUE;
  }

  for (int i = 0; i < UNIQ_LOOP_LIMIT; ++i) {
    static char buildID[30];
    sprintf(buildID, "%s_%i", aGREMilestone, i);

    PR_snprintf(regfile, MAXPATHLEN, "%s/%s.conf", root, buildID);

    if (MakeConfFile(regfile, greHome, aProperties, aPropertiesLen,
                     aGREMilestone)) {
      PR_Write(fd, buildID, strlen(buildID));
      return PR_TRUE;
    }
  }

  return PR_FALSE;
}

void
UnregisterXULRunner(PRBool aRegisterGlobally, nsIFile* aLocation,
                    const char *aGREMilestone)
{
  nsresult rv;

  char root[MAXPATHLEN] = "S:gre.d";

  if (!aRegisterGlobally) {
    char *home = PR_GetEnv("HOME");
    if (!home || !*home)
      return;

    PR_snprintf(root, MAXPATHLEN, "%s/.gre.d", home);
  }

  nsCOMPtr<nsIFile> savedInfoFile;
  aLocation->Clone(getter_AddRefs(savedInfoFile));
  nsCOMPtr<nsILocalFile> localSaved (do_QueryInterface(savedInfoFile));
  if (!localSaved)
    return;

  const char *infoname = aRegisterGlobally ? kRegFileGlobal : kRegFileUser;
  localSaved->AppendNative(nsDependentCString(infoname));

  PRFileDesc* fd = nsnull;
  rv = localSaved->OpenNSPRFileDesc(PR_RDONLY, 0, &fd);
  if (NS_FAILED(rv)) {
    // XXX report error?
    return;
  }

  char keyName[MAXPATHLEN];
  PRInt32 r = PR_Read(fd, keyName, MAXPATHLEN);
  PR_Close(fd);

  localSaved->Remove(PR_FALSE);

  if (r <= 0)
    return;

  keyName[r] = '\0';

  char regFile[MAXPATHLEN];
  PR_snprintf(regFile, MAXPATHLEN, "%s/%s.conf", root, keyName);

  nsCOMPtr<nsILocalFile> lf;
  rv = NS_NewNativeLocalFile(nsDependentCString(regFile), PR_FALSE,
                             getter_AddRefs(lf));
  if (NS_FAILED(rv))
    return;

  nsINIParser p;
  rv = p.Init(lf);
  if (NS_FAILED(rv))
    return;

  rv = p.GetString(aGREMilestone, "GRE_PATH", root, MAXPATHLEN);
  if (NS_FAILED(rv))
    return;

  rv = NS_NewNativeLocalFile(nsDependentCString(root), PR_TRUE,
                             getter_AddRefs(lf));
  if (NS_FAILED(rv))
    return;

  PRBool eq;
  if (NS_SUCCEEDED(aLocation->Equals(lf, &eq)) && eq)
    PR_Delete(regFile);
}
