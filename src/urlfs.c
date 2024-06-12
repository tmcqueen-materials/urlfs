#define FUSE_USE_VERSION 29
#define _FILE_OFFSET_BITS 64

#include <fuse.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <linux/fs.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

/* Config file layout:

/path/to/index/file
Header1: Value1
Header2: Value2

*/

/* Index file layout:

D<tab>/path/to
F<tab>/path/to/file<tab>https://url/to/file
F<tab>/path/to/other<tab>https://url/to/other

*/

#define TDIR 0
#define TFILE 1

//#define FAKE_SIZE 9223372036854775807L
#define FAKE_SIZE 0
#define MAX_REDIRS 64

char config[PATH_MAX] = {0};
char idxfil[PATH_MAX] = {0};

struct curl_slist *hdrs = NULL;

#define F_KEEP 0x0001

typedef struct index_s {
    int type;
    char *file;
    char *url;
    long size;
    char allowrr;
    int flags;
    struct index_s *next;
} file_t;

file_t *fileindex = NULL;

struct hdrblock {
    int newloc;
    struct index_s *idx;
};

struct block {
    size_t offset;
    size_t pos;
    size_t buffer_size;
    size_t buffer_pos;
    void *buffer;
};

static file_t *getFileByName(const char *n) {
    for (file_t *scan = fileindex; scan; scan = scan->next) {
        if(!strcmp(scan->file, n)) {
            return scan;
        }
    }
    return NULL;
}

// Although we are read-only, we need these internal helpers to allow
// adding of files and directory from the index file

static file_t *createFileNode(const char *path, const char *url, int type) {
    file_t *file = (file_t *)malloc(sizeof(file_t));
    file->file = strdup(path);
    if (url == NULL) {
        file->url = NULL;
    } else {
        file->url = strdup(url);
    }
    file->size = -1;
    file->allowrr = 0;
    file->type = type;
    file->flags = 0;
    file->next = NULL;

    if (fileindex == NULL) {
        fileindex = file;
        return file;
    }

    for (file_t *scan = fileindex; scan; scan = scan->next) {
        if (scan->next == NULL) {
            scan->next = file;
            return file;
        }
    }
    free(file->file);
    if (file->url) free(file->url);
    free(file);
    return NULL;
}

static void deleteFileNode(file_t *file) {
    if (file != NULL) {
        if (file == fileindex) {
            fileindex = file->next;
        } else {
            for (file_t *scan = fileindex; scan; scan = scan->next) {
                if (scan->next == file) {
                    scan->next = file->next;
                }
            }
        }

        free(file->file);
        if (file->url != NULL) free(file->url);
        free(file);
    }
}

static file_t *createDirectory(const char *path) {
    file_t *file = getFileByName(path);
    if (file != NULL) {
        return NULL;
    } 

    file = createFileNode(path, NULL, TDIR);
    return file;
}

static file_t *createFile(const char *path, const char *url) {
    file_t *file = getFileByName(path);
    if (file != NULL) {
        return NULL;
    } 

    file = createFileNode(path, url, TFILE);
    return file;
}

//

static size_t getBlock(void *data, size_t size, size_t nmemb, void *userp) {
    struct block *block = (struct block *)userp;
    size_t nmemb_new = nmemb;
    if (block->pos + nmemb_new <= block->offset) {
        // Do nothing, not at starting offet yet
        block->pos += nmemb_new;
        return nmemb;
    } else if (block->pos < block->offset) {
        // need to skip some bytes.
        data += (block->offset - block->pos);
        nmemb_new -= (block->offset - block->pos);
        block->pos = block->offset;
    }
    // Copy away, up to size of buffer
    if (block->buffer_pos + nmemb_new <= block->buffer_size) {
        memcpy(block->buffer + block->buffer_pos, data, nmemb_new);
        block->buffer_pos += nmemb_new;
    } else if (block->buffer_pos < block->buffer_size) {
        memcpy(block->buffer + block->buffer_pos, data, block->buffer_size - block->buffer_pos);
        block->buffer_pos = block->buffer_size;
    }
    block->pos += nmemb_new;
    return nmemb;
}

static size_t getHeader(char *b, size_t size, size_t nitems, void *ud) {
    struct hdrblock *hdr = (struct hdrblock *)ud;
    struct index_s *file = hdr->idx;

    if (strncasecmp(b, "content-length: ", 16) == 0) {
        file->size = strtol(b + 16, 0, 10);
    }

    if (strncasecmp(b, "accept-ranges: bytes", 20) == 0) {
        file->allowrr = 1;
    }

    if (strncasecmp(b, "location: ", 10) == 0) {
        char *trim = strtok(b + 10, " \t\r\n");
        if (file->url != NULL) free(file->url);
        file->url = strdup(trim);
        hdr->newloc = 1;
    }

    return nitems;
}

static long getFileSize(struct index_s *file) {
    int redirs = 0;
    struct hdrblock hdrblock;
    hdrblock.newloc = 1;
    hdrblock.idx = file;
    while (hdrblock.newloc && redirs < MAX_REDIRS) {
        redirs++;
        hdrblock.newloc = 0;
        file->size = -1; // Not all URLs return a content length. So size might still be -1 after this call.
        CURL *curl = curl_easy_init();
        curl_easy_setopt(curl, CURLOPT_URL, file->url);
        if (hdrs)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_HEADER, 0L);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); // we manually chase location to avoid spurious lengths from a 30x redirect return
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdrblock);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, getHeader);
        int res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            curl_easy_strerror(res);
        }
        curl_easy_cleanup(curl);
    }
    return 0;
}

static int fuse_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

    filler(buffer, ".", NULL, 0);
    filler(buffer, "..", NULL, 0);

    int l = strlen(path);
    char search[l + 2];
    strcpy(search, path);
    if (search[l-1] != '/') {
        strcat(search, "/");
        l++;
    }

    if (fileindex == NULL) return 0;

    for (file_t *scan = fileindex; scan; scan = scan->next) {
        if (strncmp(scan->file, search, l) == 0) {
            char *sub = strdup(scan->file + l);
            char *fn = strtok(sub, "/");
            char *more = strtok(NULL, "/");
            if (more == NULL) {
                filler(buffer, fn, NULL, 0);
            }
            free(sub);
        }
    }

    return 0;
}

static int fuse_getattr(const char *path, struct stat *st) {

    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atime = time( NULL );
    st->st_mtime = time( NULL );

    if ((strcmp(path, ".") == 0) || (strcmp(path, "..") == 0)) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }


    file_t *file = getFileByName(path);

    if (file == NULL) return -ENOENT;

    if (file->type == TDIR) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    if (file->type == TFILE) {
        st->st_mode = S_IFREG | 0444;
        st->st_nlink = 1;

        if (file->url == NULL) {
            st->st_size = 0;
        } else {
            if (file->size == -1) {
                getFileSize(file);
            }
            if (file->size == -1) {
                st->st_size = FAKE_SIZE;
            } else {
                st->st_size = file->size;
            }
        }
        return 0;
    }

    return -1;
}

static int fuse_open(const char *path, struct fuse_file_info *fi) {
    struct index_s *file = getFileByName(path);

    if (file == NULL)
        return -ENOENT;

    if (file->size == -1)
        getFileSize(file);
    if (file->size == -1)
        fi->direct_io = 1; // must use direct IO for files of unknown length to make sure reader gets all bytes requested

    return 0;
}

static int fuse_release(const char *path, struct fuse_file_info *fi) {
    return 0;
}

static int fuse_read( const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi )
{
    char range[100];
    sprintf(range, "%ld-%ld", offset, offset + size - 1);
    file_t *file = getFileByName(path);
    if (file == NULL) return -ENOENT;
    if (file->type == TDIR) return -EISDIR;
    if (file->url == NULL) return -ENXIO;

    struct block block;

    if (file->allowrr)
        block.offset = 0;
    else
        block.offset = offset;
    block.pos = 0;
    block.buffer_size = size;
    block.buffer = buffer;
    block.buffer_pos = 0;

    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, file->url);
    if (hdrs)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    if (file->allowrr)
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &block);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, getBlock);
    int res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_strerror(res);
    }
    curl_easy_cleanup(curl);

    // If we read te full remote resource, we know its size,
    // so update accordingly.
    if (!file->allowrr)
      file->size = block.pos;

    return block.buffer_pos;
}

static int fuse_listxattr(const char *path, char *buffer, size_t len) {
    if (strcmp(path, ".") == 0) return 0;
    if (strcmp(path, "..") == 0) return 0;

    file_t *file = getFileByName(path);
    if (file == NULL) return -ENOENT;
    if (file->type != TFILE) return 0;
    if (file->url == NULL) return 0;

    if (len == 0) {
        return 12;
    }

    memcpy(buffer, "url\0refresh\0size\0", 17);
    return 12;
}

static int fuse_getxattr(const char *path, const char *attr, char *buffer, size_t len) {

    if (strcmp(path, "/") == 0) return 0;
    if (strcmp(path, ".") == 0) return 0;
    if (strcmp(path, "..") == 0) return 0;

    file_t *file = getFileByName(path);
    if (file == NULL) return -ENOENT;
    if (file->type != TFILE) return 0;


    if (strcmp(attr, "url") == 0) {
        if (file->url == NULL) return 0;

        if (len == 0) {
            return strlen(file->url);
        }

        memcpy(buffer, file->url, strlen(file->url));
        return strlen(file->url);
    }

    if (strcmp(attr, "refresh") == 0) {
        if (len == 0) {
            return 1;
        }
        buffer[0] = '0';
        return 1;
    }

    if (strcmp(attr, "size") == 0) {
        char temp[10];
        snprintf(temp, 10, "%lu", file->size);
        if (len == 0) {
            return strlen(temp);
        }
        memcpy(buffer, temp, strlen(temp));
        return strlen(temp);
    }

    return 0;
}

static int fuse_statfs(const char *path, struct statvfs *st) {

    unsigned long count = 0;
    unsigned long size = 0;
    for (file_t *scan = fileindex; scan; scan = scan->next) {
        if (scan->type == TFILE) {
            if (scan->size == -1) {
                getFileSize(scan);
            }
            if (scan->size >= 0)
                size += scan->size;
            count++;
        }
    }


    st->f_bsize = 1024;
    st->f_blocks = size/1024;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_files = count;
    st->f_ffree = 0;

    return 0;
}

static void fuse_load_index() {
    FILE *f = fopen(idxfil, "r");
    if (f == NULL) return;

    if (fileindex != NULL) {
        for (file_t *scan = fileindex; scan; scan = scan->next) {
            scan->flags &= ~F_KEEP;
        }
    }

    char entry[32768];
    while (fgets(entry, 32768, f) != NULL) {
        char *type = strtok(entry, "\t\r\n");
        char *name = strtok(NULL, "\t\r\n");
        char *url = strtok(NULL, "\t\r\n");

        if (name == NULL) continue;

        file_t *file = getFileByName(name);
        if (type[0] == 'F') {
            if (file != NULL) {
                file->type = TFILE;
                if (file->url) free(file->url);
                file->url = NULL;
                if (url != NULL) {
                    file->url = strdup(url);
                }
                file->size = -1;
            } else {
                file = createFile(name, url);
            }

            file->flags |= F_KEEP;
        } else if (type[0] == 'D') {
            if (file != NULL) {
                file->type = TDIR;
                if (file->url) {
                    free(file->url);
                    file->url = NULL;
                }
            } else {
                file = createDirectory(name);
            }
            file->flags |= F_KEEP;
        }
    }
    fclose(f);

    for (file_t *scan = fileindex; scan; scan = scan->next) {
        if ((scan->flags & F_KEEP) == 0) {
            deleteFileNode(scan);
        }
    }
}

static void fuse_load_config() {
    FILE *f = fopen(config, "r");
    if (f == NULL) return;

    if (hdrs) {
        curl_slist_free_all(hdrs);
        hdrs = NULL;
    }

    int first = 0;
    char entry[32768];
    while (fgets(entry, 32768, f) != NULL) {
        char *val = strtok(entry, "\t\r\n");
        if (val == NULL) continue;
        if (!first && strlen(val) > 0 && val[0] != '#') {
            strcpy(idxfil, val);
            first = 1;
        } else if (strlen(val) > 0 && val[0] != '#') {
            curl_slist_append(hdrs, val);
        }
    }
    fclose(f);

    if (first)
        fuse_load_index();
}


static void *fuse_init(struct fuse_conn_info *conn) {
    fuse_load_config();
    return NULL;
}

void sigusr1(int sig) {
    fuse_load_config();
}

static struct fuse_operations operations = {
    .readdir	= fuse_readdir,
    .getattr	= fuse_getattr,
    .open       = fuse_open,
    .release    = fuse_release,
    .read       = fuse_read,
    .listxattr  = fuse_listxattr,
    .getxattr   = fuse_getxattr,
    .init       = fuse_init,
    .statfs     = fuse_statfs,
};

int main(int argc, char **argv) {

    signal(SIGUSR1, sigusr1);

    int opt;

    char *args[argc];
    int argno = 0;
    args[argno++] = argv[0];
    char *mp = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            args[argno++] = "-o";
            i++;
            args[argno++] = argv[i];
            continue;
        }

        if (strcmp(argv[i], "-f") == 0) {
            args[argno++] = "-f";
            continue;
        }

        if (argv[i][0] == '-') {
            printf("Unknown option: %s\n", argv[i]);
            return -1;
        }

        if (config[0] == 0) {
            char *c = realpath(argv[i], config);
        } else if (mp == NULL) {
            mp = argv[i];
        } else {
            printf("Extra invalid argument on command line\n");
            return -1;
        }
    }

    if (config == NULL || mp == NULL) {
        printf("Usage: %s [-o option...] configfile mountpoint\n", argv[0]);
        return -1;
    }

    args[argno++] = mp;

    return fuse_main( argno, args, &operations, NULL );
}
