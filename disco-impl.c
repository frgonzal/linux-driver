/* Necessary includes for device drivers */
#include <linux/init.h>
/* #include <linux/config.h> */
#include <linux/module.h>
#include <linux/kernel.h> /* printk() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/fs.h> /* everything... */
#include <linux/errno.h> /* error codes */
#include <linux/types.h> /* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h> /* O_ACCMODE */
#include <linux/uaccess.h> /* copy_from/to_user */

#include "kmutex.h"

MODULE_LICENSE("Dual BSD/GPL");

/* Declaration of disco.c functions */
static void disco_exit(void);
static int disco_init(void);

static int disco_open(struct inode *inode, struct file *filp);
static int disco_release(struct inode *inode, struct file *filp);
static ssize_t disco_read(struct file *filp, char *buf, size_t count, loff_t *f_pos);
static ssize_t disco_write(struct file *filp, const char *buf, size_t count, loff_t *f_pos);


/* Structure that declares the usual file */
/* access functions */
struct file_operations disco_fops = {
    read: disco_read,
    write: disco_write,
    open: disco_open,
    release: disco_release
};

/* Declaration of the init and exit functions */
module_init(disco_init);
module_exit(disco_exit);

/** Definition of macros */
#define TRUE 1
#define FALSE 0

/* Buffer to store data */
#define MAX_SIZE 8

/* Definition of states */
enum { OPEN, CLOSED };
enum { WAITING, READY};

/** Definition of the pipe */
typedef struct {
    char buffer[MAX_SIZE];
    int size;
    int status_writer, status_reader;
    KMutex mutex;
    KCondition cond;
} Pipe;

/** Definition of the waiting node */
typedef struct {
    int status;
    struct file *reader;
} WaitingNode;


/* Global variables of the driver */
static int disco_major = 61;

/** Pending reader */
static WaitingNode* pend_reader;

/* Mutex and condition variables */
static KMutex mutex;
static KCondition cond;



static int disco_init(void) {
    int rc = register_chrdev(disco_major, "disco", &disco_fops);
    if (rc < 0) {
        printk("<1>disco: cannot obtain major number %d\n", disco_major);
        return rc;
    }

    m_init(&mutex);
    c_init(&cond);
    pend_reader = 0;

    printk("<1>Inserting disco module\n");
    return 0;
}


static void disco_exit(void) {
    unregister_chrdev(disco_major, "syncread");
    printk("<1>Removing disco module\n");
}


static int disco_open(struct inode *inode, struct file *filp) {
    int rc= 0;

    m_lock(&mutex);

    if (filp->f_mode & FMODE_WRITE) {
        printk("<1>open request for write\n");

        while (!pend_reader) {
            if (c_wait(&cond, &mutex)) {
                rc= -EINTR;
                goto epilog;
            }
        }

        WaitingNode* reader_node = pend_reader;
        pend_reader = 0;

        Pipe* pipe = kmalloc(sizeof(Pipe), GFP_KERNEL);
        pipe->status_writer = OPEN;
        pipe->status_reader = CLOSED;
        pipe->size = 0;
        memset(pipe->buffer, 0, MAX_SIZE);
        m_init(&pipe->mutex);
        c_init(&pipe->cond);

        filp->private_data = reader_node->reader->private_data = pipe;
        reader_node->status = READY;

        printk("<1>open for write successful\n");

    } else if (filp->f_mode & FMODE_READ) {
        printk("<1>open request for read\n");

        while (pend_reader) {
            if (c_wait(&cond, &mutex)) {
                rc= -EINTR;
                goto epilog;
            }
        }

        WaitingNode node = { .status = WAITING, .reader = filp};
        pend_reader = &node;

        c_broadcast(&cond);
        while(node.status == WAITING) {
            if (c_wait(&cond, &mutex)) {
                pend_reader = 0;
                rc= -EINTR;
                goto epilog;
            }
        }

        Pipe* pipe = filp->private_data;
        pipe->status_reader = OPEN;

        printk("<1>open for read\n");
    }

epilog:
    c_broadcast(&cond);
    m_unlock(&mutex);
    return rc;
}

static int disco_release(struct inode *inode, struct file *filp) {
    Pipe* pipe = filp->private_data;

    m_lock(&pipe->mutex);

    if (filp->f_mode & FMODE_WRITE) {
        printk("<1>close for write\n");
        pipe->status_writer = CLOSED;
    } else if(filp->f_mode & FMODE_READ) {
        printk("<1>close for read\n");
        pipe->status_reader = CLOSED;
    }

    if(pipe->status_reader == CLOSED && pipe->status_writer == CLOSED){
        m_unlock(&pipe->mutex);
        kfree(pipe);
    }else{
        c_signal(&pipe->cond);
        m_unlock(&pipe->mutex);
    }

    printk("<1>close successful\n");
    return 0;
}

static ssize_t disco_read(struct file *filp, char *buf, size_t ucount, loff_t *f_pos) {
    printk("<1>read request\n");

    int count= ucount;
    Pipe* pipe = filp->private_data;
    int out = *f_pos;

    m_lock(&pipe->mutex);

    while (pipe->size==0 && pipe->status_writer == OPEN) {
        if (c_wait(&pipe->cond, &pipe->mutex)) {
            count= -EINTR;
            goto epilog;
        }
    }

    if (count > pipe->size)
        count= pipe->size;

    for (int k= 0; k<count; k++) {
        if (copy_to_user(buf+k, pipe->buffer+out, 1)!=0) {
            count= -EFAULT;
            goto epilog;
        }
        out = (out+1)%MAX_SIZE;
        pipe->size--;
    }
    *f_pos = out;

epilog:
    c_broadcast(&pipe->cond);
    m_unlock(&pipe->mutex);
    printk("<1>read successful\n");
    return count;
}

static ssize_t disco_write(struct file *filp, const char *buf, size_t ucount, loff_t *f_pos) {
    printk("<1>write request\n");
    int count= ucount;

    Pipe* pipe = filp->private_data;
    int in = *f_pos;

    m_lock(&pipe->mutex);


    for (int k= 0; k<count; k++) {
        while (pipe->size==MAX_SIZE) {
            c_signal(&pipe->cond);
            if (c_wait(&pipe->cond, &pipe->mutex)) {
                count= -EINTR;
                goto epilog;
            }
        }

        if (copy_from_user(pipe->buffer+in, buf+k, 1)!=0) {
            count= -EFAULT;
            goto epilog;
        }
        in = (in+1)%MAX_SIZE;
        pipe->size++;
    }
    *f_pos = in;

epilog:
    c_broadcast(&pipe->cond);
    m_unlock(&pipe->mutex);
    printk("<1>write successful\n");
    return count;
}

