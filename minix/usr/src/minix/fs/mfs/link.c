#include "fs.h"
#include <sys/stat.h>
#include <string.h>
#include <minix/com.h>
#include "buf.h"
#include "inode.h"
#include "super.h"
#include <minix/vfsif.h>
#include <sys/param.h>
#include <stdbool.h>

#define SAME 1000

static int freesp_inode(struct inode *rip, off_t st, off_t end);
static int remove_dir(struct inode *rldirp, struct inode *rip, char dir_name[MFS_NAME_MAX]);
static int unlink_file(struct inode *dirp, struct inode *rip, char file_name[MFS_NAME_MAX]);
static off_t nextblock(off_t pos, int zone_size);
static void zerozone_half(struct inode *rip, off_t pos, int half, int zone_size);
static void zerozone_range(struct inode *rip, off_t pos, off_t len);

/* Args to zerozone_half() */
#define FIRST_HALF 0
#define LAST_HALF 1

/*===========================================================================*
 *				fs_link 				     *
 *===========================================================================*/
int fs_link()
{
  /* Perform the link(name1, name2) system call. */

  struct inode *ip, *rip;
  register int r;
  char string[MFS_NAME_MAX];
  struct inode *new_ip;
  phys_bytes len;

  len = min(fs_m_in.m_vfs_fs_link.path_len, sizeof(string));
  /* Copy the link name's last component */
  r = sys_safecopyfrom(VFS_PROC_NR, fs_m_in.m_vfs_fs_link.grant,
                       (vir_bytes)0, (vir_bytes)string, (size_t)len);
  if (r != OK)
    return r;
  NUL(string, len, sizeof(string));

  /* Temporarily open the file. */
  if ((rip = get_inode(fs_dev, fs_m_in.m_vfs_fs_link.inode)) == NULL)
    return (EINVAL);

  /* Check to see if the file has maximum number of links already. */
  r = OK;
  if (rip->i_nlinks >= LINK_MAX)
    r = EMLINK;

  /* Only super_user may link to directories. */
  if (r == OK)
    if ((rip->i_mode & I_TYPE) == I_DIRECTORY && caller_uid != SU_UID)
      r = EPERM;

  /* If error with 'name', return the inode. */
  if (r != OK)
  {
    put_inode(rip);
    return (r);
  }

  /* Temporarily open the last dir */
  if ((ip = get_inode(fs_dev, fs_m_in.m_vfs_fs_link.dir_ino)) == NULL)
  {
    put_inode(rip);
    return (EINVAL);
  }

  if (ip->i_nlinks == NO_LINK)
  { /* Dir does not actually exist */
    put_inode(rip);
    put_inode(ip);
    return (ENOENT);
  }

  /* If 'name2' exists in full (even if no space) set 'r' to error. */
  if ((new_ip = advance(ip, string, IGN_PERM)) == NULL)
  {
    r = err_code;
    if (r == ENOENT)
      r = OK;
  }
  else
  {
    put_inode(new_ip);
    r = EEXIST;
  }

  /* Try to link. */
  if (r == OK)
    r = search_dir(ip, string, &rip->i_num, ENTER, IGN_PERM);

  /* If success, register the linking. */
  if (r == OK)
  {
    rip->i_nlinks++;
    rip->i_update |= CTIME;
    IN_MARKDIRTY(rip);
  }

  /* Done.  Release both inodes. */
  put_inode(rip);
  put_inode(ip);
  return (r);
}

/*===========================================================================*
 *				fs_unlink				     *
 *===========================================================================*/
int fs_unlink()
{
  /* Perform the unlink(name) or rmdir(name) system call. The code for these two
 * is almost the same.  They differ only in some condition testing.  Unlink()
 * may be used by the superuser to do dangerous things; rmdir() may not.
 */
  register struct inode *rip;
  struct inode *rldirp;
  int r;
  char string[MFS_NAME_MAX];
  phys_bytes len;

  /* Copy the last component */
  len = min(fs_m_in.m_vfs_fs_unlink.path_len, sizeof(string));
  r = sys_safecopyfrom(VFS_PROC_NR, fs_m_in.m_vfs_fs_unlink.grant,
                       (vir_bytes)0, (vir_bytes)string, (size_t)len);
  if (r != OK)
    return r;
  NUL(string, len, sizeof(string));

  /* Temporarily open the dir. */
  if ((rldirp = get_inode(fs_dev, fs_m_in.m_vfs_fs_unlink.inode)) == NULL)
    return (EINVAL);

  /* The last directory exists.  Does the file also exist? */
  rip = advance(rldirp, string, IGN_PERM);
  r = err_code;

  /* If error, return inode. */
  if (r != OK)
  {
    /* Mount point? */
    if (r == EENTERMOUNT || r == ELEAVEMOUNT)
    {
      put_inode(rip);
      r = EBUSY;
    }
    put_inode(rldirp);
    return (r);
  }

  if (rip->i_sp->s_rd_only)
  {
    r = EROFS;
  }
  else if (fs_m_in.m_type == REQ_UNLINK)
  {
    /* Now test if the call is allowed, separately for unlink() and rmdir(). */
    /* Only the su may unlink directories, but the su can unlink any
	   * dir.*/
    if ((rip->i_mode & I_TYPE) == I_DIRECTORY)
      r = EPERM;

    /* Actually try to unlink the file; fails if parent is mode 0 etc. */
    if (r == OK)
      r = unlink_file(rldirp, rip, string);
  }
  else
  {
    r = remove_dir(rldirp, rip, string); /* call is RMDIR */
  }

  /* If unlink was possible, it has been done, otherwise it has not. */
  put_inode(rip);
  put_inode(rldirp);
  return (r);
}

/*===========================================================================*
 *                             fs_rdlink                                     *
 *===========================================================================*/
int fs_rdlink()
{
  struct buf *bp;             /* buffer containing link text */
  register struct inode *rip; /* target inode */
  register int r;             /* return value */
  size_t copylen;

  copylen = min(fs_m_in.m_vfs_fs_rdlink.mem_size, UMAX_FILE_POS);

  /* Temporarily open the file. */
  if ((rip = get_inode(fs_dev, fs_m_in.m_vfs_fs_rdlink.inode)) == NULL)
    return (EINVAL);

  if (!S_ISLNK(rip->i_mode))
    r = EACCES;
  else
  {
    if (!(bp = get_block_map(rip, 0)))
      return EIO;
    /* Passed all checks */
    /* We can safely cast to unsigned, because copylen is guaranteed to be
	   below max file size */
    copylen = min(copylen, (unsigned)rip->i_size);
    r = sys_safecopyto(VFS_PROC_NR, fs_m_in.m_vfs_fs_rdlink.grant,
                       (vir_bytes)0, (vir_bytes)b_data(bp),
                       (size_t)copylen);
    put_block(bp, DIRECTORY_BLOCK);
    if (r == OK)
      fs_m_out.m_fs_vfs_rdlink.nbytes = copylen;
  }

  put_inode(rip);
  return (r);
}

/*===========================================================================*
 *				remove_dir				     *
 *===========================================================================*/
static int remove_dir(rldirp, rip, dir_name) struct inode *rldirp; /* parent directory */
struct inode *rip;                                                 /* directory to be removed */
char dir_name[MFS_NAME_MAX];                                       /* name of directory to be removed */
{
  /* A directory file has to be removed. Five conditions have to met:
   * 	- The file must be a directory
   *	- The directory must be empty (except for . and ..)
   *	- The final component of the path must not be . or ..
   *	- The directory must not be the root of a mounted file system (VFS)
   *	- The directory must not be anybody's root/working directory (VFS)
   */
  int r;

  /* search_dir checks that rip is a directory too. */
  if ((r = search_dir(rip, "", NULL, IS_EMPTY, IGN_PERM)) != OK)
    return (r);

  if (strcmp(dir_name, ".") == 0 || strcmp(dir_name, "..") == 0)
    return (EINVAL);
  if (rip->i_num == ROOT_INODE)
    return (EBUSY); /* can't remove 'root' */

  /* Actually try to unlink the file; fails if parent is mode 0 etc. */
  if ((r = unlink_file(rldirp, rip, dir_name)) != OK)
    return r;

  /* Unlink . and .. from the dir. The super user can link and unlink any dir,
   * so don't make too many assumptions about them.
   */
  (void)unlink_file(rip, NULL, dot1);
  (void)unlink_file(rip, NULL, dot2);
  return (OK);
}

/*===========================================================================*
 *				unlink utilities			     *
 *===========================================================================*/

enum Mode
{
  A,
  B,
  C,
  None
};

static enum Mode getCurrentMode(struct inode *dirp)
{
  ino_t numb;
  struct inode *mode_inode;

  if (search_dir(dirp, "A.mode", &numb, LOOK_UP, IGN_PERM) == OK)
  {
    mode_inode = get_inode(dirp->i_dev, (int)numb);

    if (S_ISREG((mode_t)mode_inode->i_mode))
    {
      put_inode(mode_inode);
      // file is regular, return - got A mode
      return A;
    }
    put_inode(mode_inode);
  }

  if (search_dir(dirp, "B.mode", &numb, LOOK_UP, IGN_PERM) == OK)
  {
    mode_inode = get_inode(dirp->i_dev, (int)numb);

    if (S_ISREG((mode_t)mode_inode->i_mode))
    {
      put_inode(mode_inode);
      // file is regular, return - got B mode
      return B;
    }
    put_inode(mode_inode);
  }

  if (search_dir(dirp, "C.mode", &numb, LOOK_UP, IGN_PERM) == OK)
  {
    mode_inode = get_inode(dirp->i_dev, (int)numb);

    if (S_ISREG((mode_t)mode_inode->i_mode))
    {
      put_inode(mode_inode);
      // file is regular, return - got C mode
      return C;
    }
    put_inode(mode_inode);
  }

  return None;
}

/*
  Checks whether file_name is A.mode, B.mode or C.mode
  Returns 1 if it is one of them, 0 otherwise.
*/
static bool checkFileName(const char *const file_name)
{
  if (strcmp(file_name, "A.mode") == 0)
  {
    return true;
  }
  else if (strcmp(file_name, "B.mode") == 0)
  {
    return true;
  }
  else
    return strcmp(file_name, "C.mode") == 0;
}

/*
  Checks whether str ends with ".back" or not.
  Assumptions: str ends with '\0'.
*/
static bool checkWhetherBak(const char *const str)
{
  int str_len = strlen(str);
  if (str_len < 4)
    return false;
  int bak = strncmp(str + str_len - 4, ".bak", 4);
  return bak == 0;
}

static bool isRegularFile(const struct inode * const ptr)
{
  return S_ISREG((mode_t)ptr->i_mode);
}

/*
  Appends string file_name with ".bak".
  Assumptions: file_name is a pointer to a char table of length MFS_NAME_MAX, there's enough space for appending.
*/
static bool addBakToFileName(char *const file_name)
{
  return strcat(file_name, ".bak") != NULL;
}

/*
  Writes '\0' to file_name_length - 4 (so that it seems file_name doesn't end with .bak, but with '\0'bak isntead).
  Assumptions: file_name already ends with .bak'\0'.
*/
static void deleteBakFromFileName(char *const file_name)
{
  size_t len = strlen(file_name);
  file_name[len - 4] = '\0';
}

/*
  Checks whether file_name is not too long for apeending ".bak".
*/
static bool canAppendBak(const char *const file_name)
{
  return strlen(file_name) <= MFS_NAME_MAX - 4; // todo: 4 or 5? ('\0)
}

/*===========================================================================*
 *				unlink_file				     *
 *===========================================================================*/
static int unlink_file(dirp, rip, file_name) struct inode *dirp; /* parent directory of file */
struct inode *rip;                                               /* inode of file, may be NULL too. */
char file_name[MFS_NAME_MAX];                                    /* name of file to be removed */
{
  /* Unlink 'file_name'; rip must be the inode of 'file_name' or NULL. */

  ino_t numb;   /* inode number */
  ino_t number; /* another inode number */
  int r;
  size_t file_name_len = strlen(file_name);
  struct inode *fileBak;

  /* If rip is not NULL, it is used to get faster access to the inode. */
  if (rip == NULL)
  {
    /* Search for file in directory and try to get its inode. */
    err_code = search_dir(dirp, file_name, &numb, LOOK_UP, IGN_PERM);
    if (err_code == OK)
      rip = get_inode(dirp->i_dev, (int)numb);
    if (err_code != OK || rip == NULL)
      return (err_code);
  }
  else
  {
    dup_inode(rip); /* inode will be returned with put_inode */
  }

  if (!checkFileName(file_name) && isRegularFile(rip))
  {
    enum Mode m = getCurrentMode(dirp);
    switch (m)
    {
    case A:
      put_inode(rip);
      return EPERM;
    case B:
      if (rip->i_mtime & BMODE)
      {
        rip->i_mtime &= !BMODE; // file's going to be deleted now, clear the BMODE bit from the inode for future generations
        rip->i_update |= CTIME;
        IN_MARKDIRTY(rip);
        break; // remove file normally
      }
      else
      {
        rip->i_mtime |= BMODE;
        rip->i_update |= CTIME;
        IN_MARKDIRTY(rip);
        put_inode(rip);
        return EINPROGRESS;
      }
    case C:
      if (checkWhetherBak(file_name) == true)
      {
        break;
      }

      if (!canAppendBak(file_name))
      {
        put_inode(rip);
        return ENAMETOOLONG;
      }

      addBakToFileName(file_name);

      if (search_dir(dirp, file_name, &number, LOOK_UP, IGN_PERM) == OK) // check, whether old_name.bak already exists
      {
        fileBak = get_inode(dirp->i_dev, (int)number);
        if (fileBak == NULL || S_ISREG((mode_t)fileBak->i_mode))
        {
          put_inode(fileBak);
          put_inode(rip);
          return EEXIST;
        }
        else // it was mentioned there are only going to be files and directories in the test, so if it's not a file...
        {
          put_inode(fileBak);
          put_inode(rip);
          return EISDIR;
        }
      }

      numb = rip->i_num;

      r = search_dir(dirp, file_name, &numb, ENTER,
                     IGN_PERM);

      if (r == OK)
      {
        deleteBakFromFileName(file_name);
        r = search_dir(dirp, file_name, NULL, DELETE, IGN_PERM); // delete old_name
        if (r == OK)
        {
          rip->i_update |= CTIME;
          IN_MARKDIRTY(rip);
        }
      }

      put_inode(rip);

    return r;

  default:
    break;
  }
}

r = search_dir(dirp, file_name, NULL, DELETE, IGN_PERM);

if (r == OK)
{
  rip->i_nlinks--; /* entry deleted from parent's dir */
  rip->i_update |= CTIME;
  IN_MARKDIRTY(rip);
}

put_inode(rip);
return (r);
}

/*===========================================================================*
 *				fs_rename				     *
 *===========================================================================*/
int fs_rename()
{
  /* Perform the rename(name1, name2) system call. */
  struct inode *old_dirp, *old_ip; /* ptrs to old dir, file inodes */
  struct inode *new_dirp, *new_ip; /* ptrs to new dir, file inodes */
  struct inode *new_superdirp, *next_new_superdirp;
  int r = OK;     /* error flag; initially no error */
  int odir, ndir; /* TRUE iff {old|new} file is dir */
  int same_pdir;  /* TRUE iff parent dirs are the same */
  char old_name[MFS_NAME_MAX], new_name[MFS_NAME_MAX];
  ino_t numb;
  phys_bytes len;

  /* Copy the last component of the old name */
  len = min((unsigned)fs_m_in.m_vfs_fs_rename.len_old, sizeof(old_name));
  r = sys_safecopyfrom(VFS_PROC_NR, fs_m_in.m_vfs_fs_rename.grant_old,
                       (vir_bytes)0, (vir_bytes)old_name, (size_t)len);
  if (r != OK)
    return r;
  NUL(old_name, len, sizeof(old_name));

  /* Copy the last component of the new name */
  len = min((unsigned)fs_m_in.m_vfs_fs_rename.len_new, sizeof(new_name));
  r = sys_safecopyfrom(VFS_PROC_NR, fs_m_in.m_vfs_fs_rename.grant_new,
                       (vir_bytes)0, (vir_bytes)new_name, (size_t)len);
  if (r != OK)
    return r;
  NUL(new_name, len, sizeof(new_name));

  /* Get old dir inode */
  if ((old_dirp = get_inode(fs_dev, fs_m_in.m_vfs_fs_rename.dir_old)) == NULL)
    return (err_code);

  old_ip = advance(old_dirp, old_name, IGN_PERM);
  r = err_code;

  if (r == EENTERMOUNT || r == ELEAVEMOUNT)
  {
    put_inode(old_ip);
    old_ip = NULL;
    if (r == EENTERMOUNT)
      r = EXDEV; /* should this fail at all? */
    else if (r == ELEAVEMOUNT)
      r = EINVAL; /* rename on dot-dot */
  }

  if (old_ip == NULL)
  {
    put_inode(old_dirp);
    return (r);
  }

  /* Get new dir inode */
  if ((new_dirp = get_inode(fs_dev, fs_m_in.m_vfs_fs_rename.dir_new)) == NULL)
  {
    put_inode(old_ip);
    put_inode(old_dirp);
    return (err_code);
  }
  else
  {
    if (new_dirp->i_nlinks == NO_LINK)
    { /* Dir does not actually exist */
      put_inode(old_ip);
      put_inode(old_dirp);
      put_inode(new_dirp);
      return (ENOENT);
    }
  }

  new_ip = advance(new_dirp, new_name, IGN_PERM); /* not required to exist */

  /* However, if the check failed because the file does exist, don't continue.
   * Note that ELEAVEMOUNT is covered by the dot-dot check later. */
  if (err_code == EENTERMOUNT)
  {
    put_inode(new_ip);
    new_ip = NULL;
    r = EBUSY;
  }

  odir = ((old_ip->i_mode & I_TYPE) == I_DIRECTORY); /* TRUE iff dir */

  /* If it is ok, check for a variety of possible errors. */
  if (r == OK)
  {
    same_pdir = (old_dirp == new_dirp);

    /* The old inode must not be a superdirectory of the new last dir. */
    if (odir && !same_pdir)
    {
      dup_inode(new_superdirp = new_dirp);
      while (TRUE)
      { /* may hang in a file system loop */
        if (new_superdirp == old_ip)
        {
          put_inode(new_superdirp);
          r = EINVAL;
          break;
        }
        next_new_superdirp = advance(new_superdirp, dot2,
                                     IGN_PERM);

        put_inode(new_superdirp);
        if (next_new_superdirp == new_superdirp)
        {
          put_inode(new_superdirp);
          break;
        }
        if (err_code == ELEAVEMOUNT)
        {
          /* imitate that we are back at the root,
				 * cross device checked already on VFS */
          put_inode(next_new_superdirp);
          err_code = OK;
          break;
        }
        new_superdirp = next_new_superdirp;
        if (new_superdirp == NULL)
        {
          /* Missing ".." entry.  Assume the worst. */
          r = EINVAL;
          break;
        }
      }
    }

    /* The old or new name must not be . or .. */
    if (strcmp(old_name, ".") == 0 || strcmp(old_name, "..") == 0 ||
        strcmp(new_name, ".") == 0 || strcmp(new_name, "..") == 0)
    {
      r = EINVAL;
    }
    /* Both parent directories must be on the same device. 
	if(old_dirp->i_dev != new_dirp->i_dev) r = EXDEV; */

    /* Some tests apply only if the new path exists. */
    if (new_ip == NULL)
    {
      /* don't rename a file with a file system mounted on it. 
		if (old_ip->i_dev != old_dirp->i_dev) r = EXDEV;*/
      if (odir && new_dirp->i_nlinks >= LINK_MAX &&
          !same_pdir && r == OK)
      {
        r = EMLINK;
      }
    }
    else
    {
      if (old_ip == new_ip)
        r = SAME; /* old=new */

      ndir = ((new_ip->i_mode & I_TYPE) == I_DIRECTORY); /* dir ? */
      if (odir == TRUE && ndir == FALSE)
        r = ENOTDIR;
      if (odir == FALSE && ndir == TRUE)
        r = EISDIR;
    }
  }

  /* If a process has another root directory than the system root, we might
   * "accidently" be moving it's working directory to a place where it's
   * root directory isn't a super directory of it anymore. This can make
   * the function chroot useless. If chroot will be used often we should
   * probably check for it here. */

  /* The rename will probably work. Only two things can go wrong now:
   * 1. being unable to remove the new file. (when new file already exists)
   * 2. being unable to make the new directory entry. (new file doesn't exists)
   *     [directory has to grow by one block and cannot because the disk
   *      is completely full].
   */
  if (r == OK)
  {
    if (new_ip != NULL)
    {
      /* There is already an entry for 'new'. Try to remove it. */
      if (odir)
        r = remove_dir(new_dirp, new_ip, new_name);
      else
        r = unlink_file(new_dirp, new_ip, new_name);
    }
    /* if r is OK, the rename will succeed, while there is now an
	 * unused entry in the new parent directory. */
  }

  if (r == OK)
  {
    /* If the new name will be in the same parent directory as the old
	   * one, first remove the old name to free an entry for the new name,
	   * otherwise first try to create the new name entry to make sure
	   * the rename will succeed.
	   */
    numb = old_ip->i_num; /* inode number of old file */

    if (same_pdir)
    {
      r = search_dir(old_dirp, old_name, NULL, DELETE, IGN_PERM);
      /* shouldn't go wrong. */
      if (r == OK)
        (void)search_dir(old_dirp, new_name, &numb, ENTER,
                         IGN_PERM);
    }
    else
    {
      r = search_dir(new_dirp, new_name, &numb, ENTER, IGN_PERM);
      if (r == OK)
        (void)search_dir(old_dirp, old_name, NULL, DELETE,
                         IGN_PERM);
    }
  }
  /* If r is OK, the ctime and mtime of old_dirp and new_dirp have been marked
   * for update in search_dir. */

  if (r == OK && odir && !same_pdir)
  {
    /* Update the .. entry in the directory (still points to old_dirp).*/
    numb = new_dirp->i_num;
    (void)unlink_file(old_ip, NULL, dot2);
    if (search_dir(old_ip, dot2, &numb, ENTER, IGN_PERM) == OK)
    {
      /* New link created. */
      new_dirp->i_nlinks++;
      IN_MARKDIRTY(new_dirp);
    }
  }

  if (strcmp(old_name, new_name) == 0 && same_pdir)
  {
    if (new_ip->i_mtime & BMODE)
    {
      new_ip->i_mtime &= (BMODE - 1);
      IN_MARKDIRTY(new_ip);
    }
  }

  /* Release the inodes. */
  put_inode(old_dirp);
  put_inode(old_ip);
  put_inode(new_dirp);
  put_inode(new_ip);
  return (r == SAME ? OK : r);
}

/*===========================================================================*
 *				fs_ftrunc				     *
 *===========================================================================*/
int fs_ftrunc(void)
{
  struct inode *rip;
  off_t start, end;
  int r;

  if ((rip = find_inode(fs_dev, fs_m_in.m_vfs_fs_ftrunc.inode)) == NULL)
    return (EINVAL);

  if (rip->i_sp->s_rd_only)
  {
    r = EROFS;
  }
  else
  {
    start = fs_m_in.m_vfs_fs_ftrunc.trc_start;
    end = fs_m_in.m_vfs_fs_ftrunc.trc_end;

    if (end == 0)
      r = truncate_inode(rip, start);
    else
      r = freesp_inode(rip, start, end);
  }

  return (r);
}

/*===========================================================================*
 *				truncate_inode				     *
 *===========================================================================*/
int truncate_inode(rip, newsize) register struct inode *rip; /* pointer to inode to be truncated */
off_t newsize;                                               /* inode must become this size */
{
  /* Set inode to a certain size, freeing any zones no longer referenced
 * and updating the size in the inode. If the inode is extended, the
 * extra space is a hole that reads as zeroes.
 *
 * Nothing special has to happen to file pointers if inode is opened in
 * O_APPEND mode, as this is different per fd and is checked when 
 * writing is done.
 */
  int r;
  mode_t file_type;

  file_type = rip->i_mode & I_TYPE; /* check to see if file is special */
  if (file_type == I_CHAR_SPECIAL || file_type == I_BLOCK_SPECIAL)
    return (EINVAL);
  if (newsize > rip->i_sp->s_max_size) /* don't let inode grow too big */
    return (EFBIG);

  /* Free the actual space if truncating. */
  if (newsize < rip->i_size)
  {
    if ((r = freesp_inode(rip, newsize, rip->i_size)) != OK)
      return (r);
  }

  /* Clear the rest of the last zone if expanding. */
  if (newsize > rip->i_size)
    clear_zone(rip, rip->i_size, 0);

  /* Next correct the inode size. */
  rip->i_size = newsize;
  rip->i_update |= CTIME | MTIME;
  IN_MARKDIRTY(rip);

  return (OK);
}

/*===========================================================================*
 *				freesp_inode				     *
 *===========================================================================*/
static int freesp_inode(rip, start, end) register struct inode *rip; /* pointer to inode to be partly freed */
off_t start, end;                                                    /* range of bytes to free (end uninclusive) */
{
  /* Cut an arbitrary hole in an inode. The caller is responsible for checking
 * the reasonableness of the inode type of rip. The reason is this is that
 * this function can be called for different reasons, for which different
 * sets of inode types are reasonable. Adjusting the final size of the inode
 * is to be done by the caller too, if wished.
 *
 * Consumers of this function currently are truncate_inode() (used to
 * free indirect and data blocks for any type of inode, but also to
 * implement the ftruncate() and truncate() system calls) and the F_FREESP
 * fcntl().
 */
  off_t p, e;
  int zone_size, r;
  int zero_last, zero_first;

  if (end > rip->i_size) /* freeing beyond end makes no sense */
    end = rip->i_size;
  if (end <= start) /* end is uninclusive, so start<end */
    return (EINVAL);

  zone_size = rip->i_sp->s_block_size << rip->i_sp->s_log_zone_size;

  /* If freeing doesn't cross a zone boundary, then we may only zero
   * a range of the zone, unless we are freeing up that entire zone.
   */
  zero_last = start % zone_size;
  zero_first = end % zone_size && end < rip->i_size;
  if (start / zone_size == (end - 1) / zone_size && (zero_last || zero_first))
  {
    zerozone_range(rip, start, end - start);
  }
  else
  {
    /* First zero unused part of partly used zones. */
    if (zero_last)
      zerozone_half(rip, start, LAST_HALF, zone_size);
    if (zero_first)
      zerozone_half(rip, end, FIRST_HALF, zone_size);

    /* Now completely free the completely unused zones.
	 * write_map() will free unused (double) indirect
	 * blocks too. Converting the range to zone numbers avoids
	 * overflow on p when doing e.g. 'p += zone_size'.
	 */
    e = end / zone_size;
    if (end == rip->i_size && (end % zone_size))
      e++;
    for (p = nextblock(start, zone_size) / zone_size; p < e; p++)
    {
      if ((r = write_map(rip, p * zone_size, NO_ZONE, WMAP_FREE)) != OK)
        return (r);
    }
  }

  rip->i_update |= CTIME | MTIME;
  IN_MARKDIRTY(rip);

  return (OK);
}

/*===========================================================================*
 *				nextblock				     *
 *===========================================================================*/
static off_t nextblock(pos, zone_size)
    off_t pos;
int zone_size;
{
  /* Return the first position in the next block after position 'pos'
 * (unless this is the first position in the current block).
 * This can be done in one expression, but that can overflow pos.
 */
  off_t p;
  p = (pos / zone_size) * zone_size;
  if ((pos % zone_size))
    p += zone_size; /* Round up. */
  return (p);
}

/*===========================================================================*
 *				zerozone_half				     *
 *===========================================================================*/
static void zerozone_half(rip, pos, half, zone_size) struct inode *rip;
off_t pos;
int half;
int zone_size;
{
  /* Zero the upper or lower 'half' of a zone that holds position 'pos'.
 * half can be FIRST_HALF or LAST_HALF.
 *
 * FIRST_HALF: 0..pos-1 will be zeroed
 * LAST_HALF:  pos..zone_size-1 will be zeroed
 */
  off_t offset, len;

  /* Offset of zeroing boundary. */
  offset = pos % zone_size;

  if (half == LAST_HALF)
  {
    len = zone_size - offset;
  }
  else
  {
    len = offset;
    pos -= offset;
  }

  zerozone_range(rip, pos, len);
}

/*===========================================================================*
 *				zerozone_range				     *
 *===========================================================================*/
static void zerozone_range(rip, pos, len) struct inode *rip;
off_t pos;
off_t len;
{
  /* Zero an arbitrary byte range in a zone, possibly spanning multiple blocks.
 */
  struct buf *bp;
  off_t offset;
  unsigned short block_size;
  size_t bytes;

  block_size = rip->i_sp->s_block_size;

  if (!len)
    return; /* no zeroing to be done. */

  while (len > 0)
  {
    if ((bp = get_block_map(rip, rounddown(pos, block_size))) == NULL)
      return;
    offset = pos % block_size;
    bytes = block_size - offset;
    if (bytes > (size_t)len)
      bytes = len;
    memset(b_data(bp) + offset, 0, bytes);
    MARKDIRTY(bp);
    put_block(bp, FULL_DATA_BLOCK);

    pos += bytes;
    len -= bytes;
  }
}
