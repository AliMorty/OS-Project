//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <stddef.h>
#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"
#include "x86.h"


// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
    int fd;
    struct file *f;

    if (argint(n, &fd) < 0)
        return -1;
    if (fd < 0 || fd >= NOFILE || (f = proc->ofile[fd]) == 0)
        return -1;
    if (pfd)
        *pfd = fd;
    if (pf)
        *pf = f;
    return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
    int fd;

    for (fd = 0; fd < NOFILE; fd++)
    {
        if (proc->ofile[fd] == 0)
        {
            proc->ofile[fd] = f;
            return fd;
        }
    }
    return -1;
}

int
sys_dup(void)
{
    struct file *f;
    int fd;

    if (argfd(0, 0, &f) < 0)
        return -1;
    if ((fd = fdalloc(f)) < 0)
        return -1;
    filedup(f);
    return fd;
}

int
sys_read(void)
{
    struct file *f;
    int n;
    char *p;

    if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
        return -1;
    return fileread(f, p, n);
}

int
sys_write(void)
{
    struct file *f;
    int n;
    char *p;

    if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
        return -1;
    return filewrite(f, p, n);
}

int
sys_close(void)
{
    int fd;
    struct file *f;

    if (argfd(0, &fd, &f) < 0)
        return -1;
    proc->ofile[fd] = 0;
    fileclose(f);
    return 0;
}

int
sys_fstat(void)
{
    struct file *f;
    struct stat *st;

    if (argfd(0, 0, &f) < 0 || argptr(1, (void *) &st, sizeof(*st)) < 0)
        return -1;
    return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
    char name[DIRSIZ], *new, *old;
    struct inode *dp, *ip;

    if (argstr(0, &old) < 0 || argstr(1, &new) < 0)
        return -1;

    begin_op();
    if ((ip = namei(old)) == 0)
    {
        end_op();
        return -1;
    }

    ilock(ip);
    if (ip->type == T_DIR)
    {
        iunlockput(ip);
        end_op();
        return -1;
    }

    ip->nlink++;
    iupdate(ip);
    iunlock(ip);

    if ((dp = nameiparent(new, name)) == 0)
        goto bad;
    ilock(dp);
    if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0)
    {
        iunlockput(dp);
        goto bad;
    }
    iunlockput(dp);
    iput(ip);

    end_op();

    return 0;

    bad:
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
    end_op();
    return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
    int off;
    struct dirent de;

    for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de))
    {
        if (readi(dp, (char *) &de, off, sizeof(de)) != sizeof(de))
            panic("isdirempty: readi");
        if (de.inum != 0)
            return 0;
    }
    return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
    struct inode *ip, *dp;
    struct dirent de;
    char name[DIRSIZ], *path;
    uint off;

    if (argstr(0, &path) < 0)
        return -1;

    begin_op();
    if ((dp = nameiparent(path, name)) == 0)
    {
        end_op();
        return -1;
    }

    ilock(dp);

    // Cannot unlink "." or "..".
    if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
        goto bad;

    if ((ip = dirlookup(dp, name, &off)) == 0)
        goto bad;
    ilock(ip);

    if (ip->nlink < 1)
        panic("unlink: nlink < 1");
    if (ip->type == T_DIR && !isdirempty(ip))
    {
        iunlockput(ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (writei(dp, (char *) &de, off, sizeof(de)) != sizeof(de))
        panic("unlink: writei");
    if (ip->type == T_DIR)
    {
        dp->nlink--;
        iupdate(dp);
    }
    iunlockput(dp);

    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);

    end_op();

    return 0;

    bad:
    iunlockput(dp);
    end_op();
    return -1;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
    uint off;
    struct inode *ip, *dp;
    char name[DIRSIZ];

    if ((dp = nameiparent(path, name)) == 0)
        return 0;
    ilock(dp);

    if ((ip = dirlookup(dp, name, &off)) != 0)
    {
        iunlockput(dp);
        ilock(ip);
        if (type == T_FILE && ip->type == T_FILE)
            return ip;
        iunlockput(ip);
        return 0;
    }

    if ((ip = ialloc(dp->dev, type)) == 0)
        panic("create: ialloc");

    ilock(ip);
    ip->major = major;
    ip->minor = minor;
    ip->nlink = 1;
    iupdate(ip);

    if (type == T_DIR)
    {  // Create . and .. entries.
        dp->nlink++;  // for ".."
        iupdate(dp);
        // No ip->nlink++ for ".": avoid cyclic ref count.
        if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
            panic("create dots");
    }

    if (dirlink(dp, name, ip->inum) < 0)
        panic("create: dirlink");

    iunlockput(dp);

    return ip;
}



int
sys_open(void)
{
    char *path;
    int fd, omode;
    struct file *f;
    struct inode *ip;

    if (argstr(0, &path) < 0 || argint(1, &omode) < 0)
        return -1;

    begin_op();

    if (omode & O_CREATE)
    {
        ip = create(path, T_FILE, 0, 0);
        if (ip == 0)
        {
            end_op();
            return -1;
        }
    } else
    {
        if ((ip = namei(path)) == 0)
        {
            end_op();
            return -1;
        }
        ilock(ip);
        if (ip->type == T_DIR && omode != O_RDONLY)
        {
            iunlockput(ip);
            end_op();
            return -1;
        }
    }

    if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
    {
        if (f)
            fileclose(f);
        iunlockput(ip);
        end_op();
        return -1;
    }
    iunlock(ip);
    end_op();

    f->type = FD_INODE;
    f->ip = ip;
    f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    return fd;
}

int
sys_mkdir(void)
{
    char *path;
    struct inode *ip;

    begin_op();
    if (argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0)
    {
        end_op();
        return -1;
    }
    iunlockput(ip);
    end_op();
    return 0;
}

int
sys_mknod(void)
{
    struct inode *ip;
    char *path;
    int len;
    int major, minor;

    begin_op();
    if ((len = argstr(0, &path)) < 0 ||
        argint(1, &major) < 0 ||
        argint(2, &minor) < 0 ||
        (ip = create(path, T_DEV, major, minor)) == 0)
    {
        end_op();
        return -1;
    }
    iunlockput(ip);
    end_op();
    return 0;
}

int
sys_chdir(void)
{
    char *path;
    struct inode *ip;

    begin_op();
    if (argstr(0, &path) < 0 || (ip = namei(path)) == 0)
    {
        end_op();
        return -1;
    }
    ilock(ip);
    if (ip->type != T_DIR)
    {
        iunlockput(ip);
        end_op();
        return -1;
    }
    iunlock(ip);
    iput(proc->cwd);
    end_op();
    proc->cwd = ip;
    return 0;
}

int
sys_exec(void)
{
    char *path, *argv[MAXARG];
    int i;
    uint uargv, uarg;

    if (argstr(0, &path) < 0 || argint(1, (int *) &uargv) < 0)
    {
        return -1;
    }
    memset(argv, 0, sizeof(argv));
    for (i = 0; ; i++)
    {
        if (i >= NELEM(argv))
            return -1;
        if (fetchint(uargv + 4 * i, (int *) &uarg) < 0)
            return -1;
        if (uarg == 0)
        {
            argv[i] = 0;
            break;
        }
        if (fetchstr(uarg, &argv[i]) < 0)
            return -1;
    }
    return exec(path, argv);
}

int
sys_pipe(void)
{
    int *fd;
    struct file *rf, *wf;
    int fd0, fd1;

    if (argptr(0, (void *) &fd, 2 * sizeof(fd[0])) < 0)
        return -1;
    if (pipealloc(&rf, &wf) < 0)
        return -1;
    fd0 = -1;
    if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0)
    {
        if (fd0 >= 0)
            proc->ofile[fd0] = 0;
        fileclose(rf);
        fileclose(wf);
        return -1;
    }
    fd[0] = fd0;
    fd[1] = fd1;
    return 0;
}

///////////////////////////////////////////////////////////////
//////////////////////////MINE!////////////////////////////////
///////////////////////////////////////////////////////////////
int
the_opener(char *p, int om)
{
    char *path = p;
    int fd, omode = om;
    struct file *f;
    struct inode *ip;

    begin_op();

    if (omode & O_CREATE)
    {
        ip = create(path, T_FILE, 0, 0);
        if (ip == 0)
        {
            end_op();
            return -1;
        }
    } else
    {
        if ((ip = namei(path)) == 0)
        {
            end_op();
            return -1;
        }
        ilock(ip);
        if (ip->type == T_DIR && omode != O_RDONLY)
        {
            iunlockput(ip);
            end_op();
            return -1;
        }
    }

    if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
    {
        if (f)
            fileclose(f);
        iunlockput(ip);
        end_op();
        return -1;
    }
    iunlock(ip);
    end_op();

    f->type = FD_INODE;
    f->ip = ip;
    f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    return fd;
}

int
sys_isvpcb(void)
{
    int fd, fd2, file_size;
    pte_t *pte;
    uint pa, i, flag;
//    struct proc *child_proc = NULL;
    cprintf("Parent PID: %d\n",proc->pid);
//    get_proc(proc->pid + 1, &child_proc);
//    cprintf("Child PID: %d\n",child_proc->pid);
/////////////////////////Saving UVM///////////////////////////////
    //Creating file for UVM
    fd = the_opener("pages", O_CREATE | O_RDWR);
    fd2 = the_opener("flag", O_CREATE | O_RDWR);
    if (fd < 0)
    {
        cprintf("Error:Failed to create UVM file.\n");
        exit();
    } //Checking for errors in creating file
    cprintf("Created UVM file.\n");
    struct file *f = proc->ofile[fd];
    struct file *f2 = proc->ofile[fd2];

    //Coping the user virtual memory and writing to the file
    for (i = 0; i < proc->sz; i += PGSIZE)
    {
        if ((pte = ns_walkpgdir(proc->pgdir, (void *) i, 0)) == 0)
            panic("copyuvm: pte should exist.");
        if (!(*pte & PTE_P))
            panic("copyuvm: page not present.");
        pa = PTE_ADDR(*pte);
        flag = PTE_FLAGS(*pte);

        //writing to file
        file_size = filewrite(f, (char *) p2v(pa), PGSIZE);
        filewrite(f2, (char *)&flag , sizeof(uint));
        //Checking for write errors
        if (file_size != PGSIZE)
        {
            cprintf("Error:Failed to write UVM file.\n");
            exit();
        }
        cprintf("Written UVM Page %d.\n", i / PGSIZE);
    }
    proc->ofile[fd] = 0;
    proc->ofile[fd2] = 0;
    fileclose(f);
    fileclose(f2);
/////////////////////////Saving context///////////////////////////////
    //Creating file for context
    fd = the_opener("context", O_CREATE | O_RDWR);
    if (fd < 0)
    {
        cprintf("Error:Failed to create context file.\n");
        exit();
    } //Checking for errors in creating file
    cprintf("Created context file.\n");
    f = proc->ofile[fd];
    //writing to file
    file_size = filewrite(f, (char *) proc->context, sizeof(struct context));
    //Checking for write errors
    if (file_size != sizeof(struct context))
    {
        cprintf("Error:Failed to write context file.\n");
        exit();
    }
    cprintf("Written context file.\n", i / PGSIZE);
    proc->ofile[fd] = 0;
    fileclose(f);
///////////////////////////Saving trapframe/////////////////////////////////
    //Creating file for trapframe
    fd = the_opener("trapframe", O_CREATE | O_RDWR);
    if (fd < 0)
    {
        cprintf("Error:Failed to create trapframe file.\n");
        exit();
    } //Checking for errors in creating file
    cprintf("Created trapframe file.\n");
    f = proc->ofile[fd];
    //writing to file
    file_size = filewrite(f, (char *) proc->tf, sizeof(struct trapframe));
    //Checking for write errors
    if (file_size != sizeof(struct trapframe))
    {
        cprintf("Error:Failed to write trapframe file.\n");
        exit();
    }
    cprintf("Written trapframe file.\n", i / PGSIZE);
    proc->ofile[fd] = 0;
    fileclose(f);
//////////////////////////Saving proc////////////////////////////////
    //Creating file for proc
    fd = the_opener("proc", O_CREATE | O_RDWR);
    if (fd < 0)
    {
        cprintf("Error:Failed to create proc file.\n");
        exit();
    } //Checking for errors in creating file
    cprintf("Created proc file.\n");
    f = proc->ofile[fd];
    //writing to file
    file_size = filewrite(f, (char *) proc, sizeof(struct proc));
    //Checking for write errors
    if (file_size != sizeof(struct proc))
    {
        cprintf("Error:Failed to write proc file.\n");
        exit();
    }
    cprintf("Written proc file.\n", i / PGSIZE);
    proc->ofile[fd] = 0;
    fileclose(f);


    cprintf("\n*Write is done.*\n\n", i / PGSIZE);
    kill(proc->pid);
    exit();
    return 0;

}

int
sys_ildpcb(void)
{
    int pid;

    //Creating files
    int pages_fd = the_opener("pages", O_RDONLY);
    int context_fd = the_opener("context", O_RDONLY);
    int tf_fd = the_opener("trapframe", O_RDONLY);
    int proc_fd = the_opener("proc", O_RDONLY);
    int flag_fd = the_opener("flag", O_RDONLY);
    struct file *pages_f = proc->ofile[pages_fd];
    struct file *context_f = proc->ofile[context_fd];
    struct file *tf_f = proc->ofile[tf_fd];
    struct file *proc_f = proc->ofile[proc_fd];
    struct file *flag_f = proc->ofile[flag_fd];

    //Reading files
    struct context loaded_context;
    struct trapframe loaded_tf;
    struct proc loaded_proc;
    fileread(context_f, (char *) &loaded_context, sizeof(struct context));
    fileread(tf_f, (char *) &loaded_tf, sizeof(struct trapframe));
    fileread(proc_f, (char *) &loaded_proc, sizeof(struct proc));
    cprintf("Read was successful.\n");

    *loaded_proc.context=loaded_context;
    *loaded_proc.tf=loaded_tf;
    pid = load_the_proc(&loaded_proc,pages_f,flag_f);

    proc->ofile[pages_fd] = 0;
    proc->ofile[context_fd] = 0;
    proc->ofile[tf_fd] = 0;
    proc->ofile[flag_fd] = 0;
    proc->ofile[proc_fd] = 0;
    fileclose(pages_f);
    fileclose(context_f);
    fileclose(tf_f);
    fileclose(flag_f);
    fileclose(proc_f);
    return pid;
}



