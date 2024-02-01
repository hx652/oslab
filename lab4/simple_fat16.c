#include <asm-generic/errno.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/timeb.h>

#include "fat16.h"

/* FAT16 volume data with a file handler of the FAT16 image file */
// 存储 FAT 文件系统所需要的元数据的数据结构
typedef struct {
	uint32_t sector_size;           // 逻辑扇区大小（字节）
	uint32_t sec_per_clus;          // 每簇扇区数
	uint32_t reserved;              // 保留扇区数
	uint32_t fats;                  // FAT表的数量
	uint32_t dir_entries;           // 根目录项数量
	uint32_t sectors;               // 文件系统总扇区数
	uint32_t sec_per_fat;            // 每个FAT表所占扇区数

    sector_t fat_sec;               // FAT表开始扇区
    sector_t root_sec;              // 根目录区域开始扇区
    uint32_t root_sectors;          // 根目录区域扇区数
    sector_t data_sec;              // 数据区域开始扇区
    
    uint32_t clusters;              // 文件系统簇数
    uint32_t cluster_size;          // 簇大小（字节）

    uid_t fs_uid;               // 可忽略，挂载FAT的用户ID，所有文件的拥有者都显示为该用户
    gid_t fs_gid;               // 可忽略，挂载FAT的组ID，所有文件的用户组都显示为该组
    struct timespec atime;      // 可忽略，访问时间
    struct timespec mtime;      // 可忽略，修改时间
    struct timespec ctime;      // 可忽略，创建时间
} FAT16;

FAT16 meta;

#define ATTR_CONTAINS(attr, attr_name) ((attr & attr_name) != 0)

size_t sector_offset(sector_t sector) {
    return sector * meta.sector_size;
}

/**
 * @brief 簇号是否是合法的（表示正在使用的）数据簇号（在CLUSTER_MIN和CLUSTER_MAX之间）
 * 
 * @param clus 簇号
 * @return int        
 */
bool is_cluster_inuse(cluster_t clus) {
    return CLUSTER_MIN <= clus && clus <= CLUSTER_MAX;
}

sector_t cluster_first_sector(cluster_t clus) {
    assert(is_cluster_inuse(clus));
    return ((clus - 2) * meta.sec_per_clus) + meta.data_sec;
}

cluster_t sector_cluster(sector_t sec) {
    if(sec < meta.data_sec) {
        return 0;
    }
    cluster_t clus = 2 + (sec - meta.data_sec) / meta.sec_per_clus;
    assert(is_cluster_inuse(clus));
    return clus;
}

int is_cluster_end(cluster_t clus) {
    return clus >= CLUSTER_END_BOUND;
}

bool is_readonly(attr_t attr) {
    return (attr & ATTR_READONLY) != 0;
}

bool is_directory(attr_t attr) {
    return (attr & ATTR_DIRECTORY) != 0;
}

bool is_lfn(attr_t attr) {
    return attr == ATTR_LFN;
}

bool is_free(DIR_ENTRY* dir) {
    return dir->DIR_Name[0] == NAME_FREE;
}

bool is_deleted(DIR_ENTRY* dir) {
    return dir->DIR_Name[0] == NAME_DELETED;
}

bool is_valid(DIR_ENTRY* dir) {
    const uint8_t* name = dir->DIR_Name;
    attr_t attr = dir->DIR_Attr;
    return !is_lfn(attr) && name[0] != NAME_DELETED && name[0] != NAME_FREE;
}

bool is_dot(DIR_ENTRY* dir) {
    if(is_lfn(dir->DIR_Attr)) {
        return false;
    }
    const char* name = (const char *)dir->DIR_Name;
    attr_t attr = dir->DIR_Attr;
    const char DOT_NAME[] =    ".          ";
    const char DOTDOT_NAME[] = "..         ";
    return strncmp(name, DOT_NAME, FAT_NAME_LEN) == 0 || strncmp(name, DOTDOT_NAME, FAT_NAME_LEN) == 0;
}

bool path_is_root(const char* path) {
    path += strspn(path, "/");
    return *path == '\0';
}

sector_t clus_to_first_sec(cluster_t clus) {
    return meta.data_sec + (clus - 2) * meta.sec_per_clus;
}
//返回指定簇的起始sector编号
// added

/**
 * @brief 将文件名转换为FAT的 8+3 文件名格式，存储在res中，res是长度至少为11的 char 数组
 * 
 */
int to_shortname(const char* name, size_t len, char* res) {
    bool has_ext = false;
    size_t base_len = len;

    // 找到文件名基础部分和拓展名部分，注意最后一个点后是拓展名
    for(size_t i = 0; i < len; i++) {
        if(name[i] == '\0') {
            len = i;
            base_len = min(base_len, len);
            break;
        }
        const char INVALID_CHARS[] = "*?<>|\"+=,; :\\";
        if(strchr(INVALID_CHARS, name[i]) != NULL) {
            return -EINVAL;
        }
        if(name[i] == '.' && i != 0) {
            has_ext = true;
            base_len = i;
        }
    }

    // 转换文件名
    memset(res, ' ', FAT_NAME_LEN);
    for(size_t i = 0; i < base_len && i < FAT_NAME_BASE_LEN; i++) {
        res[i] = toupper(name[i]);
    }
    // 0xe5用来代表删除，如果首个字母为0xe5,需要转换为0x05
    res[0] = (res[0] == 0xe5) ? 0x05 : res[0];

    // 转换拓展名
    if(has_ext) {
        const char* ext = name + base_len + 1;
        size_t ext_len = len - base_len - 1;
        for(size_t i = 0; i < ext_len && i < FAT_NAME_EXT_LEN; i++) {
            res[FAT_NAME_BASE_LEN + i] = toupper(ext[i]);
        }
    }
	return 0;
}

/**
 * @brief 将FAT的 8+3 文件名格式转换为普通文件名，存储在res中，len是res的长度
 */
int to_longname(const uint8_t fat_name[11], char* res, size_t len) {
    len --;  // last char for '\0' 
    size_t i = 0;
    while(i < len && i < FAT_NAME_BASE_LEN) {
        if(fat_name[i] == ' ') {
            break;
        }
        res[i] = tolower(fat_name[i]);
        i++;
    }

    if(fat_name[FAT_NAME_BASE_LEN] != ' ') {
        res[i++] = '.';
        for(size_t j = FAT_NAME_BASE_LEN; i < len && j < FAT_NAME_LEN; j++) {
            if(fat_name[j] == ' ') {
                break;
            }
            res[i] = tolower(fat_name[j]);
            i++;
        }
    }

    res[i] = '\0';
    return i;
}

// 比较长文件名是否和 dir 目录项中的文件匹配
bool check_name(const char* name, size_t len, const DIR_ENTRY* dir) {
    char fatname[11];
    to_shortname(name, len, fatname);
    return strncmp(fatname, (const char *)dir->DIR_Name, FAT_NAME_LEN) == 0;
}

/**
 * @brief 读取簇号为 clus 对应的 FAT 表项
 * 
 * @param clus 簇号
 * @return cluster_t FAT 返回值：对应 FAT 表项的值
 */
cluster_t read_fat_entry(cluster_t clus)
{
    char sector_buffer[PHYSICAL_SECTOR_SIZE];
    // TODO1.4: 读取簇号为 clus 对应的 FAT 表项，步骤如下：
    // 1. 计算簇号 clus 对应的 FAT 表项的偏移量，并计算表项所在的扇区号
    // 2. 使用 sector_read 函数读取该扇区
    // 3. 计算簇号 clus 对应的 FAT 表项在该扇区中的偏移量
    // 4. 从该偏移量处读取对应表项的值，并返回
    /** Your Code Here ... **/

    sector_t sec_num = meta.fat_sec + clus / (meta.sector_size / 2);
    //计算对应的fat表项位于哪个sector
    off_t offset_in_sector = (clus * 2) % (meta.sector_size);
    //计算对应的fat表项在sector内的偏移量

    sector_read(sec_num, sector_buffer);
    cluster_t *temp = (cluster_t *)(sector_buffer + offset_in_sector);
    return *temp;
}


/**
 * @brief 用于表示目录项查找结果的结构体
 * 
 */
typedef struct {
    DIR_ENTRY dir;      // 找到的目录项
    sector_t sector;    // 目录项所在的扇区
    size_t offset;      // 目录项在扇区中的偏移量
} DirEntrySlot;

/**
 * @brief 在 from_sector 开始的连续 sectors_count 个扇区中查找 name 对应的目录项
 *        找到对应目录项时返回 FIND_EXIST
 *        未找到对应目录项，但找到了空槽返回 FIND_EMPTY
 *        未找到对应目录项，且扇区都已满时返回 FIND_FULL
 *        出现其它错误，返回负数
 * @param name              要查找的文件名
 * @param len               文件名长度
 * @param from_sector       要查找的第一个扇区号
 * @param sectors_count     要查找的扇区数
 * @param slot              找到的目录项，参考对 DirEntrySlot 的注释
 * @return long 
 */
int find_entry_in_sectors(const char* name, size_t len, 
            sector_t from_sector, size_t sectors_count, 
            DirEntrySlot* slot) {
    char buffer[PHYSICAL_SECTOR_SIZE];
    // 对每一个待查找的扇区：
    for(size_t i = 0; i < sectors_count; i++) {
        // TODO1.3: 读取每一个扇区扇区，步骤如下：
        // 1. 使用 sector_read 函数读取从扇区号 from_sector 开始的第 i 个扇区
        // 2. 对该扇区中的每一个目录项，检查是否是待查找的目录项（注意检查目录项是否合法）
        
        sector_read(from_sector+i, buffer);
        for(size_t off = 0; off < meta.sector_size; off += DIR_ENTRY_SIZE) {
            DIR_ENTRY* dir = (DIR_ENTRY*)(buffer + off);
            // 3. 如果是待查找的目录项，将该目录项的信息填入 slot 中，并返回 FIND_EXIST
            // 4. 如果不是待查找的目录项，检查该目录项是否为空，如果为空，将该目录项的信息填入 slot 中，并返回 FIND_EMPTY
            // 5. 如果不是待查找的目录项，且该扇区中的所有目录项都不为空，返回 FIND_FULL
            if (is_lfn(dir->DIR_Attr)) {
                continue;
            }//当前目录项是lfn
            if (!is_deleted(dir)) {
                if (check_name(name, len, dir)) {
                    slot->dir = *dir;
                    slot->sector=from_sector+i;
                    slot->offset=off;
                    return FIND_EXIST;
                }
            }
            if (is_free(dir)) {
                slot->dir=*dir;
                slot->sector=from_sector+i;
                slot->offset=off;
                return FIND_EMPTY;
            }
        }
    }
    return FIND_FULL;
}


/**
 * @brief 找到path所对应路径的目录项，如果最后一级路径不存在，则找到能创建最后一集文件/目录的空目录项。（这个函数同时实现了找目录项和找空槽的功能）
 * 
 * @param path          需要查找的路径
 * @param slot          最后一级找到的目录项，参考对 DirEntrySlot 的注释
 * @param remains       path 中未找到的部分
 * @return int 成功返回0，失败返回错误代码的负值，可能的错误参见brief部分。
 */
int find_entry_internal(const char* path, DirEntrySlot* slot, const char** remains) {
    printf("in find_entry_internal\n");
    printf("path %s\n",path);
    *remains = path;
    *remains += strspn(*remains, "/");    // 跳过开头的'/'

    unsigned level = 0;
    cluster_t clus = CLUSTER_END;   // 当前查找到的目录项开始的簇号
    int state = FIND_EXIST;
    // 如果 remains 不为空，说明还有未找到的层级
    while (**remains != '\0' && state == FIND_EXIST) {
        size_t len = strcspn(*remains, "/"); // 目前要搜索的文件名长度
        // *remains 开始的，长为 len 的字符串是当前要搜索的文件名
        printf("remians is %s, len is %lu\n",*remains,len);

        if(level == 0) {
            // 如果是第一级，需要从根目录开始搜索
            // TODO1.1: 设置根目录的扇区号和扇区数（请给下面两个变量赋值，根目录的扇区号和扇区数可以在 meta 里的字段找到。）
            sector_t root_sec;
            size_t nsec;

            root_sec = meta.root_sec;
            nsec = meta.root_sectors;

            // 使用 find_entry_in_sectors 寻找相应的目录项
            state = find_entry_in_sectors(*remains, len, root_sec, nsec, slot); 
            if(state != FIND_EXIST) {
                // 根目录项中没找到第一级路径，直接返回
                printf("out find_entry_internal\n");
                return state;
            }
        } else {
            // 不是第一级，在目录对应的簇中寻找（在上一级中已将clus设为第一个簇）
            while (is_cluster_inuse(clus)) {    // 依次查找每个簇
                // TODO1.2: 在 clus 对应的簇中查找每个目录项。
                // 你可以使用 state = find_entry_in_sectors(.....)， 参数参考第一级中是如何查找的。

                sector_t clus_sec = cluster_first_sector(clus);
                //当前簇的起始sector编号
                state = find_entry_in_sectors(*remains, len, clus_sec, meta.sec_per_clus, slot);
                printf("in clus %u, ret from find_entry_in_sectors is %d\n",clus,state);

                if(state < 0) { // 出现错误
                    printf("out find_entry_internal\n");
                    return state;
                } else if(state == FIND_EXIST || state == FIND_EMPTY) {
                    break;  // 该级找到了，或者已经找完了有内容的项，不需要往后继续查找该级后面的簇
                }
                clus = read_fat_entry(clus);    // 记得实现该函数
            }
        }  // 大括号位置错误已改正。
        // 下一级目录开始位置
        const char* next_level = *remains + len;
        next_level += strspn(next_level, "/");

        if(state == FIND_EXIST) {
            // 该级找到的情况，remains后移至下一级
            level++;
            *remains = next_level;
            clus = slot->dir.DIR_FstClusLO;
        }

        if(*next_level != '\0') {
            // 不是最后一级，且没找到
            if(state != FIND_EXIST) {
                return -ENOENT;
            }

            // 不是最后一级，且不是目录
            if(!is_directory(slot->dir.DIR_Attr)) {
                return -ENOTDIR;
            }
        }
    }

    printf("out find_entry_internal\n");
    return state;
}

/**
 * @brief 读目录、读文件时使用，找到path所对应路径的目录项。其实只是包装了一下 find_entry_internal
 * 
 * @param path 
 * @param slot 
 * @return int 
 */
int find_entry(const char* path, DirEntrySlot* slot) {
    const char* remains = NULL;
    printf("in find_entry\n");
    int ret = find_entry_internal(path, slot, &remains);
    if(ret < 0) {
        return ret;
    }
    if(ret == FIND_EXIST) {
        printf("out find_entry, have found %s\n",path);
        return 0;
    }
    return -ENOENT;
}


/**
 * @brief 创建目录、创建文件时使用，找到一个空槽，并且顺便检查是否有重名文件/目录。
 * 
 * @param path 
 * @param slot 
 * @param last_name 
 * @return int 
 */
int find_empty_slot(const char* path, DirEntrySlot *slot, const char** last_name) {
    int ret = find_entry_internal(path, slot, last_name);
    printf("in find_empty_slot\n");
    printf("ret is %d, remains is %s\n",ret,*last_name);
    if(ret < 0) {
        printf("out find_empty_slot\n");
        return ret;
    }
    if(ret == FIND_EXIST) { // 找到重名文件，返回文件已存在
        printf("out find_empty_slot\n");
        return -EEXIST;
    }
    if(ret == FIND_FULL) {  // 找不到空槽，返回目录已满
        printf("out find_empty_slot\n");
        return -ENOSPC;
    }
    printf("out find_empty_slot\n");
    return 0;
}

mode_t get_mode_from_attr(uint8_t attr) {
    mode_t mode = 0;
    mode |= is_readonly(attr) ? S_IRUGO : S_NORMAL;
    mode |= is_directory(attr) ? S_IFDIR : S_IFREG;
    return mode;
}

void time_fat_to_unix(struct timespec* ts, uint16_t date, uint16_t time, uint16_t acc_time) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = (date >> 9) + 80;           // 7位年份
    t.tm_mon  = ((date >> 5) & 0xf) - 1;     // 4位月份
    t.tm_mday = date & 0x1f;              // 5位日期

    t.tm_hour = time >> 11;                 // 5位小时
    t.tm_min  = (time >> 5) & 0x3f;          // 6位分钟
    t.tm_sec  = (time & 0x1f) * 2;           // 5位秒，需要乘2，精确到2秒

    ts->tv_sec = mktime(&t) + (acc_time / 100);
    ts->tv_nsec = (acc_time % 100) * 10000000;
}

void time_unix_to_fat(const struct timespec* ts, uint16_t* date, uint16_t* time, uint8_t* acc_time) {
    struct tm* t = gmtime(&(ts->tv_sec));
    *date = 0;
    *date |= ((t->tm_year - 80) << 9);
    *date |= ((t->tm_mon + 1) << 5);
    *date |= t->tm_mday;
    
    if(time != NULL) {
        *time = 0;
        *time |= (t->tm_hour << 11);
        *time |= (t->tm_min << 5);
        *time |= (t->tm_sec / 2);
    }

    if(acc_time != NULL) {
        *acc_time = (t->tm_sec % 2) * 100;
        *acc_time += ts->tv_nsec / 10000000;
    }
}

// ===========================文件系统接口实现===============================

/**
 * @brief 文件系统初始化，无需修改
 * 
 * @param conn 
 * @return void* 
 */
void *fat16_init(struct fuse_conn_info * conn, struct fuse_config *config) {
    /* Reads the BPB */
    BPB_BS bpb;
    sector_read(0, &bpb);
    
    // TODO0.0: 你无需修改这部分代码，但阅读这部分，并理解这些变量的含义有助于你理解文件系统的结构
    // 请同时参考 FAT16 结构体的定义里的注释（本文件第15行开始）
    meta.sector_size = bpb.BPB_BytsPerSec;
    meta.sec_per_clus = bpb.BPB_SecPerClus;
    meta.reserved = bpb.BPB_RsvdSecCnt;
    meta.fats = bpb.BPB_NumFATS;
    meta.dir_entries = bpb.BPB_RootEntCnt;
    meta.sectors = bpb.BPB_TotSec16 != 0 ? bpb.BPB_TotSec16 : bpb.BPB_TotSec32;
    meta.sec_per_fat = bpb.BPB_FATSz16;

    meta.fat_sec = meta.reserved;
    meta.root_sec = meta.fat_sec + (meta.fats * meta.sec_per_fat);
    meta.root_sectors = (meta.dir_entries * DIR_ENTRY_SIZE) / meta.sector_size;
    meta.data_sec = meta.root_sec + meta.root_sectors;
    meta.clusters = (meta.sectors - meta.data_sec) / meta.sec_per_clus;
    meta.cluster_size = meta.sec_per_clus * meta.sector_size;

    // 以下可忽略
    meta.fs_uid = getuid();
    meta.fs_gid = getgid();

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    meta.atime = meta.mtime = meta.ctime = now;
    return NULL;
}

/**
 * @brief 释放文件系统，无需修改
 * 
 * @param data 
 */
void fat16_destroy(void *data) { }

/**
 * @brief 获取path对应的文件的属性，无需修改
 * 
 * @param path    要获取属性的文件路径
 * @param stbuf   输出参数，需要填充的属性结构体
 * @return int    成功返回0，失败返回POSIX错误代码的负值
 */
int fat16_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {

    // 清空所有属性
    memset(stbuf, 0, sizeof(struct stat));

    // 这些属性被忽略
    stbuf->st_dev = 0;
    stbuf->st_ino = 0;
    stbuf->st_nlink = 0;
    stbuf->st_rdev = 0;

    // 这些属性被提前计算好，不会改变
    stbuf->st_uid = meta.fs_uid;
    stbuf->st_gid = meta.fs_gid;
    stbuf->st_blksize = meta.cluster_size;

    // 这些属性需要根据文件设置
    // st_mode, st_size, st_blocks, a/m/ctim
    if (path_is_root(path)) {
        stbuf->st_mode = S_IFDIR | S_NORMAL;
        stbuf->st_size = 0;
        stbuf->st_blocks = 0;
        stbuf->st_atim = meta.atime;
        stbuf->st_mtim = meta.mtime;
        stbuf->st_ctim = meta.ctime;
        return 0;
    }

    DirEntrySlot slot;
    DIR_ENTRY* dir = &(slot.dir);
    int ret = find_entry(path, &slot);
    if(ret < 0) {
        return ret;
    }
    stbuf->st_mode = get_mode_from_attr(dir->DIR_Attr);
    stbuf->st_size = dir->DIR_FileSize;
    stbuf->st_blocks = dir->DIR_FileSize / PHYSICAL_SECTOR_SIZE;
    
    time_fat_to_unix(&stbuf->st_atim, dir->DIR_LstAccDate, 0, 0);
    time_fat_to_unix(&stbuf->st_mtim, dir->DIR_WrtDate, dir->DIR_WrtTime, 0);
    time_fat_to_unix(&stbuf->st_ctim, dir->DIR_CrtDate, dir->DIR_CrtTime, dir->DIR_CrtTimeTenth);
    return 0;
}

// ------------------TASK1: 读目录、读文件-----------------------------------

/**
 * @brief 读取path对应的目录，得到目录中有哪些文件，结果通过filler函数写入buffer中
 *        例如，如果path是/a/b，而/a/b下有 apple、orange、banana 三个文件，那么我们的函数中应该调用filler三次：
 *          filler(buf, "apple", NULL, 0, 0)
 *          filler(buf, "orange", NULL, 0, 0)
 *          filler(buf, "banana", NULL, 0, 0)
 *        然后返回0，这样就告诉了FUSE，/a/b目录下有这三个文件。
 * 
 * @param path    要读取目录的路径
 * @param buf     结果缓冲区
 * @param filler  用于填充结果的函数，本次实验按filler(buffer, 文件名, NULL, 0, 0)的方式调用即可。
 *                你也可以参考<fuse.h>第58行附近的函数声明和注释来获得更多信息。
 * @param offset  忽略
 * @param fi      忽略
 * @return int    成功返回0，失败返回POSIX错误代码的负值
 */
int fat16_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, 
                    struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    // 这里解释本函数的思路：
    //   1. path 是我们要读取的路径，有两种情况，path是根目录，path 不是根目录。
    //   2. 如果 path 是根目录，那么我们读取根目录区域的内容即可。
    //   3. 如果 path 不是根目录，那么我们需要找到 path 对应的目录项（这部分逻辑在find_entry和find_entry_internal两个函数中。）
    //   4. 目录项中保存了 path 对应的目录的第一个簇号，我们读取簇中每个目录项的内容即可。
    //   5. 使用 filler 函数将每个目录项的文件名写入 buf 中。
    
    bool root = path_is_root(path);
    DIR_ENTRY dir;
    cluster_t clus = CLUSTER_END;
    if(!root) {
        
        //printf("readdir is not root\n");

        DirEntrySlot slot;
        DIR_ENTRY* dir = &(slot.dir);

        // Hint: find_entry 找到路径对应的目录项，是这个函数最难的部分。
        // find_entry在后面所有函数中都会多次使用，请阅读、理解并补全它的实现。
        int ret = find_entry(path, &slot);
        if(ret < 0) {
            return ret;
        }
        clus = dir->DIR_FstClusLO;    // 不是根目录
        if(!is_directory(dir->DIR_Attr)) {
            return -ENOTDIR;
        }
    }

    // 要读的目录项的第一个簇位于 clus，请你读取该簇中的所有目录项。
    char sector_buffer[MAX_LOGICAL_SECTOR_SIZE];
    char name[MAX_NAME_LEN];
    while (root || is_cluster_inuse(clus)) {
        sector_t first_sec;
        size_t nsec;
        if(root) {
            first_sec = meta.root_sec;
            nsec = meta.root_sectors;
        } else {
            first_sec = cluster_first_sector(clus);
            nsec = meta.sec_per_clus;
        }


        // TODO1.5: 读取当前簇中每一个扇区内所有目录项，并将合法的项，使用filler函数的项的文件名写入 buf 中。
        // filler 的使用方法： filler(buf, 文件名, NULL, 0)
        // 你可以参考 find_entry_in_sectors 函数的实现。
        for(size_t i=0; i < nsec; i++) {
            sector_t sec = first_sec + i;
            sector_read(sec, sector_buffer);
            // TODO1.5: 对扇区中每个目录项：
            // 1. 确认其是否是表示文件或目录的项（排除 LFN、空项、删除项等不合法项的干扰）
            // 2. 从 FAT 文件名中，获得长文件名（可以使用提供的to_longname函数）
            // 3. 使用 filler 填入 buf
            // 4. 找到空项即可结束查找（说明后面均为空）。

            for (size_t off = 0; off < meta.sector_size; off+=DIR_ENTRY_SIZE) {
                DIR_ENTRY *cur_dir = (DIR_ENTRY *)(sector_buffer + off);
                if (!(is_valid(cur_dir))) {
                    continue;
                }
                to_longname(cur_dir->DIR_Name, name, MAX_NAME_LEN);
                filler(buf, name, NULL, 0, 0);
                
                //printf("fill filename is %s\n",name);
            }
        }

        if(root) {
            break;
        }

        clus = read_fat_entry(clus);
    }
    
    return 0;
}

/**
 * @brief 从簇 clus 的 offset 处开始读取 size 字节的数据到 data 中，并返回实际读取的字节数。
 * 
 * @param clus      要读取的簇号
 * @param offset    要读取的数据在簇中的偏移量
 * @param data      结果缓冲区
 * @param size      要读取的数据长度
 * @return int 
 */
int read_from_cluster_at_offset(cluster_t clus, off_t offset, char* data, size_t size) {
    // printf("Read clus %hd at offset %ld, size: %lu\n", clus, offset, size);
    assert(offset + size <= meta.cluster_size);  // offset + size 必须小于簇大小
    char sector_buffer[PHYSICAL_SECTOR_SIZE];

    uint32_t sec = cluster_first_sector(clus) + offset / meta.sector_size;
    size_t sec_off = offset % meta.sector_size;
    size_t pos = 0;
    while(pos < size) {
        int ret = sector_read(sec, sector_buffer);
        if(ret < 0) {
            return ret;
        }
        size_t len = min(meta.sector_size - sec_off, size - pos);
        memcpy(data + pos, sector_buffer + sec_off, len);
        pos += len;
        sec_off = 0;
        sec ++ ;
    }
    return size;
}

/**
 * @brief 从path对应的文件的offset字节处开始读取size字节的数据到buffer中，并返回实际读取的字节数。
 * Hint: 文件大小属性是Dir.DIR_FileSize。
 * 
 * @param path    要读取文件的路径
 * @param buffer  结果缓冲区
 * @param size    需要读取的数据长度
 * @param offset  要读取的数据所在偏移量
 * @param fi      忽略
 * @return int    成功返回实际读写的字符数，失败返回0。
 */
int fat16_read(const char *path, char *buffer, size_t size, off_t offset,
               struct fuse_file_info *fi) {
    printf("read(path='%s', offset=%ld, size=%lu)\n", path, offset, size);
    if(path_is_root(path)) {
        return -EISDIR;
    }

    DirEntrySlot slot;
    DIR_ENTRY* dir = &(slot.dir);
    int ret = find_entry(path, &slot);
    if(ret < 0) {
        return ret;
    }
    if(is_directory(dir->DIR_Attr)) {
        return -EISDIR;
    }
    if(offset > dir->DIR_FileSize) {
        return -EINVAL;
    }
    size = min(size, dir->DIR_FileSize - offset);


    cluster_t clus = dir->DIR_FstClusLO;
    size_t p = 0;
    // TODO1.6: clus 初始为该文件第一个簇，利用 read_from_cluster_at_offset 函数，从正确的簇中读取数据。
    // Hint: 需要注意 offset 的位置，和结束读取的位置。要读取的数据可能横跨多个簇，也可能就在一个簇的内部。
    // 你可以参考 read_from_cluster_at_offset 里，是怎么处理每个扇区的读取范围的，或者用自己的方式解决这个问题。

    // printf("file size is %u\n", dir->DIR_FileSize);

    off_t read_pointer = 0;
    off_t end_pointer = offset + size;

    // printf("end_pointer is %lu\n",end_pointer);

    // int i = 0;
    while (1) {
        if (read_pointer <= offset &&
            read_pointer + meta.cluster_size > offset) {
            break;
        }
        read_pointer += meta.cluster_size;
        clus = read_fat_entry(clus);
        // i++;
    }
    // printf("initial read_pointer is %lu, initial clus is %u\n", read_pointer,
    //      clus);

    size_t cur_size;
    size_t valid_space_in_clus = meta.cluster_size - (offset - read_pointer);
    cur_size = min(valid_space_in_clus, size);
    p += read_from_cluster_at_offset(clus, offset - read_pointer, buffer, cur_size);
    buffer += p;
    read_pointer += p;
    clus = read_fat_entry(clus);

    while (p < size) {
        if (read_pointer + meta.cluster_size < end_pointer) {
            cur_size = meta.cluster_size;
            p += read_from_cluster_at_offset(clus, 0, buffer, cur_size);
            read_pointer += cur_size;
            buffer += cur_size;
            clus = read_fat_entry(clus);
        }
        else {
            cur_size = size - p;
            p += read_from_cluster_at_offset(clus, 0, buffer, cur_size);
            read_pointer += cur_size;
            buffer += cur_size;
        }
    }
    // printf("read file done, read_pointer is %lu\n", read_pointer);

    return p;
}


// ------------------TASK2: 创建/删除文件-----------------------------------

/**
 * @brief 将 DirEntry 写入 Slot 里对应的目录项。
 *        注意，我们只能用 sector_write() 写入整个扇区的数据，所以要修改单个目录项，需要先读出整个扇区
 *        然后修改目录项对应的部分，然后将整个扇区写回。
 * @param slot 要写入的目录项，及目录项所在的扇区号和偏移量
 * @return int 
 */
int dir_entry_write(DirEntrySlot slot) {
    char sector_buffer[PHYSICAL_SECTOR_SIZE];
    // TODO2.1: 
    //  1. 读取 slot.dir 所在的扇区
    //  2. 将目录项写入buffer对应的位置（Hint: 使用memcpy）
    //  3. 将整个扇区完整写回

    sector_read(slot.sector, sector_buffer);
    memcpy(sector_buffer + slot.offset, &(slot.dir), sizeof(DIR_ENTRY));
    sector_write(slot.sector, sector_buffer);

    return 0;
}

int dir_entry_create(DirEntrySlot slot, const char *shortname, 
            attr_t attr, cluster_t first_clus, size_t file_size) {
    DIR_ENTRY* dir = &(slot.dir);
    memset(dir, 0, sizeof(DIR_ENTRY));
    
    memcpy(dir, shortname, 11);
    dir->DIR_Attr = attr;
    dir->DIR_FstClusHI = 0;
    dir->DIR_FstClusLO = first_clus;
    dir->DIR_FileSize = file_size;
    
    struct timespec ts;
    int ret = clock_gettime(CLOCK_REALTIME, &ts);
    if(ret < 0) {
        return -errno;
    }
    time_unix_to_fat(&ts, &(dir->DIR_CrtDate), &(dir->DIR_CrtTime), &(dir->DIR_CrtTimeTenth));
    time_unix_to_fat(&ts, &(dir->DIR_WrtDate), &(dir->DIR_WrtTime), NULL);
    time_unix_to_fat(&ts, &(dir->DIR_LstAccDate), NULL, NULL);

    ret = dir_entry_write(slot);
    if(ret < 0) {
        return ret;
    }
    return 0;
}


/**
 * @brief 将data写入簇号为clusterN的簇对应的FAT表项，注意要对文件系统中所有FAT表都进行相同的写入。
 * 
 * @param clus  要写入表项的簇号
 * @param data      要写入表项的数据，如下一个簇号，CLUSTER_END（文件末尾），或者0（释放该簇）等等
 * @return int      成功返回0
 */
int write_fat_entry(cluster_t clus, cluster_t data) {
    char sector_buffer[MAX_LOGICAL_SECTOR_SIZE];
    size_t clus_off = clus * sizeof(cluster_t);
    sector_t clus_sec = clus_off / meta.sector_size;
    size_t sec_off = clus_off % meta.sector_size;
    for(size_t i = 0; i < meta.fats; i++) {
        // TODO2.2: 修改第 i 个 FAT 表中，clus_sec 扇区中，sec_off 偏移处的表项，使其值为 data
        //   1. 计算第 i 个 FAT 表所在扇区，进一步计算clus应的FAT表项所在扇区
        //   2. 读取该扇区并在对应位置写入数据
        //   3. 将该扇区写回

        sector_t fat_start_sec = meta.fat_sec + i * meta.sec_per_fat;
        sector_read(fat_start_sec + clus_sec, sector_buffer);
        memcpy(sector_buffer + sec_off, &data, sizeof(cluster_t));
        sector_write(fat_start_sec + clus_sec, sector_buffer);
    }
    return 0;
}

int free_clusters(cluster_t clus) {
    while(is_cluster_inuse(clus)) {
        cluster_t next = read_fat_entry(clus);
        int ret = write_fat_entry(clus, CLUSTER_FREE);
        if(ret < 0) {
            return ret;
        }
        clus = next;
    }
    return 0;
}

static const char ZERO_SECTOR[PHYSICAL_SECTOR_SIZE] = {0};
int cluster_clear(cluster_t clus) {
    sector_t first_sec = cluster_first_sector(clus);
    for(size_t i = 0; i < meta.sec_per_clus; i++) {
        sector_t sec = first_sec + i;
        int ret = sector_write(sec, ZERO_SECTOR);
        if(ret < 0) {
            return ret;
        }
    }
    return 0;
}

/**
 * @brief 分配n个空闲簇，分配过程中将n个簇通过FAT表项连在一起，然后返回第一个簇的簇号。
 *        最后一个簇的FAT表项将会指向0xFFFF（即文件中止）。
 * @param fat16_ins 文件系统指针
 * @param n         要分配簇的个数
 * @return int      成功返回0，失败返回错误代码负值
 */
int alloc_clusters(size_t n, cluster_t* first_clus) {
    printf("in alloc_clusters, want to find %lu free cluster\n",n);
    if (n == 0)
        return CLUSTER_END;

    // 用于保存找到的n个空闲簇，另外在末尾加上CLUSTER_END，共n+1个簇号
    cluster_t *clusters = malloc((n + 1) * sizeof(cluster_t));
    size_t allocated = 0; // 已找到的空闲簇个数

    // TODO2.3: 扫描FAT表，找到n个空闲的簇，存入cluster数组。注意此时不需要修改对应的FAT表项。
    // Hint: 你可以使用 read_fat_entry 函数来读取FAT表项的值，根据该值判断簇是否空闲。

    cluster_t cur_clus;
    for (cur_clus = CLUSTER_MIN; cur_clus <= CLUSTER_MAX; cur_clus++) {
        cluster_t fat_entry = read_fat_entry(cur_clus);
        // printf("current cluster %u, read from it is %u\n", cur_clus, fat_entry);
        if (fat_entry == CLUSTER_FREE) {
            clusters[allocated] = cur_clus;
            allocated++;
            printf("find free cluster %u, total free num is %lu\n", cur_clus, allocated);
        }

        if (allocated == n) {
            printf("have found %lu free cluster\n", allocated);
            break;
        }  //已找到n个空闲簇
    }

    if(allocated != n) {  // 找不到n个簇，分配失败
        free(clusters);
        return -ENOSPC;
    }

    // 找到了n个空闲簇，将CLUSTER_END加至末尾。
    clusters[n] = CLUSTER_END;

    // TODO2.4: 修改clusters中存储的N个簇对应的FAT表项，将每个簇与下一个簇连接在一起。同时清零每一个新分配的簇。
    // 清零要分配的簇
    for(size_t i = 0; i < n; i++) {
        int ret = cluster_clear(clusters[i]);   // 请实现cluster_clear()
        if(ret < 0) {
            free(clusters);
            return ret;
        }
    }

    // TODO2.5: 连接要分配的簇的FAT表项（Hint: 使用write_fat_entry）
    // Hint: 将每个簇连接到下一个即可
    for (int i = 0; i < n; i++) {
        write_fat_entry(clusters[i], clusters[i + 1]);
    }

    *first_clus = clusters[0];
    free(clusters);
    return 0;
}


/**
 * @brief 在path对应的路径创建新文件 （请阅读函数的逻辑，补全find_empty_slot和dir_entry_create两个函数）
 * 
 * @param path    要创建的文件路径
 * @param mode    要创建文件的类型，本次实验可忽略，默认所有创建的文件都为普通文件
 * @param devNum  忽略，要创建文件的设备的设备号
 * @return int    成功返回0，失败返回POSIX错误代码的负值
 */
int fat16_mknod(const char *path, mode_t mode, dev_t dev) {
    printf("mknod(path='%s', mode=%03o, dev=%lu)\n", path, mode, dev);
    DirEntrySlot slot;
    const char* filename = NULL;
    int ret = find_empty_slot(path, &slot, &filename);
    printf("find empty done\n");
    if(ret < 0) {
        return ret;
    }
    printf("find empty slot, sector num %lu, offset in sector %lu\n",slot.sector,slot.offset);

    char shortname[11];
    ret = to_shortname(filename, MAX_NAME_LEN, shortname);
    if(ret < 0) {
        return ret;
    }
    // 这里创建文件时首簇号填了0，你可以根据自己需要修改。
    ret = dir_entry_create(slot, shortname, ATTR_REGULAR, 0, 0);
    if(ret < 0) {
        return ret;
    }
    return 0;
}

/**
 * @brief 删除path对应的文件（请阅读函数逻辑，补全free_clusters和dir_entry_write）
 * 
 * @param path  要删除的文件路径
 * @return int  成功返回0，失败返回POSIX错误代码的负值
 */
int fat16_unlink(const char *path) {
    printf("unlink(path='%s')\n", path);
    DirEntrySlot slot;
    DIR_ENTRY* dir = &(slot.dir);
    int ret = find_entry(path, &slot);
    if(ret < 0) {
        return ret;
    }
    if(is_directory(dir->DIR_Attr)) {
        return -EISDIR;
    }
    ret = free_clusters(dir->DIR_FstClusLO);
    if(ret < 0) {
        return ret;
    }
    dir->DIR_Name[0] = NAME_DELETED;
    ret = dir_entry_write(slot);
    if(ret < 0) {
        return ret;
    }
    return 0;
}

/**
 * @brief 修改path对应文件的时间戳，本次实验不做要求，可忽略该函数
 * 
 * @param path  要修改时间戳的文件路径
 * @param tv    时间戳
 * @return int 
 */
int fat16_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info* fi) {
    printf("utimens(path='%s', tv=[%ld.%09ld, %ld.%09ld])\n", path, 
                tv[0].tv_sec, tv[0].tv_nsec, tv[1].tv_sec, tv[1].tv_nsec);
    DirEntrySlot slot;
    DIR_ENTRY* dir = &(slot.dir);
    int ret = find_entry(path, &slot);
    if(ret < 0) {
        return ret;
    }

    time_unix_to_fat(&tv[1], &(dir->DIR_WrtDate), &(dir->DIR_WrtTime), NULL);
    time_unix_to_fat(&tv[0], &(dir->DIR_LstAccDate), NULL, NULL);
    ret = dir_entry_write(slot);
    if(ret < 0) {
        return ret;
    }
    
    return 0;
}

/**
 * @brief 创建path对应的文件夹
 * 
 * @param path 创建的文件夹路径
 * @param mode 文件模式，本次实验可忽略，默认都为普通文件夹
 * @return int 成功:0， 失败: POSIX错误代码的负值
 */
int fat16_mkdir(const char *path, mode_t mode) {

    // TODO2.6: 参考fat16_mknod实现，创建新目录
    // Hint: 注意设置的属性不同。
    // Hint: 新目录最开始即有两个目录项，分别是.和..，所以需要给新目录分配一个簇。
    // Hint: 你可以使用 alloc_clusters 来分配簇。

    DirEntrySlot slot;
    const char *filename = NULL;
    find_empty_slot(path, &slot, &filename);

    char shorname[11];
    to_shortname(filename, MAX_NAME_LEN, shorname);

    cluster_t first_clus;
    int ret = alloc_clusters(1, &first_clus);
    printf("ret from alloc_clusters is %d\n",ret);
    dir_entry_create(slot, shorname, ATTR_DIRECTORY, first_clus, 2*sizeof(DIR_ENTRY));

    const char DOT_NAME[] =    ".          ";
    const char DOTDOT_NAME[] = "..         ";

    // TODO2.7: 使用 dir_entry_create 创建 . 和 .. 目录项
    // Hint: 两个目录项分别在你刚刚分配的簇的前两项。
    // Hint: 记得修改下面的返回值。
    // TODO:

    DirEntrySlot slot_of_subdir;
    char name[11];
    to_shortname(DOT_NAME, 11, name);
    find_entry(path, &slot_of_subdir);
    slot_of_subdir.sector = cluster_first_sector(first_clus);
    slot_of_subdir.offset = 0;
    dir_entry_create(slot_of_subdir, name, ATTR_DIRECTORY,
                     slot_of_subdir.dir.DIR_FstClusLO, 0);

    to_shortname(DOTDOT_NAME, 11, name);
    int i = strlen(path);
    char pre_path[i];
    strcpy(pre_path, path);
    for (i = i - 1; i >= 0; i--) {
        if (path[i] == '/') {
            break;
        }
    }
    if (i == 0) {
        pre_path[i + 1] = '\0';
    } //上一级目录是根目录
    else {
        pre_path[i] = '\0';
    } //上一级目录不是根目录
    find_entry(pre_path, &slot_of_subdir);
    slot_of_subdir.sector = cluster_first_sector(first_clus);
    slot_of_subdir.offset = sizeof(DIR_ENTRY);
    dir_entry_create(slot_of_subdir, name, ATTR_DIRECTORY,
                     slot_of_subdir.dir.DIR_FstClusLO, 0);
                     
    return 0;
}



/**
 * @brief 删除path对应的文件夹
 * 
 * @param path 要删除的文件夹路径
 * @return int 成功:0， 失败: POSIX错误代码的负值
 */
int fat16_rmdir(const char *path) {
    printf("rmdir(path='%s')\n", path);
    if(path_is_root(path)) {
        return -EBUSY;
    }

    DirEntrySlot slot;
    DIR_ENTRY* dir = &(slot.dir);
    int ret = find_entry(path, &slot);
    if(ret < 0) {
        return ret;
    }
    if(!is_directory(dir->DIR_Attr)) {
        return -ENOTDIR;
    }

    // TODO: 请参考fat16_unlink实现，实现删除目录功能。
    // Hint: 你只需要删除为空的目录，不为空的目录返回 -ENOTEMPTY 错误即可。
    // Hint: 你需要类似fat16_readdir一样，读取目录项，来判断目录是否为空。
    // Hint: 空目录中也有.和..两个目录项，你要正确地忽略它们。
    // Hint: 记得修改下面的返回值

    char sec_buffer[meta.sector_size];
    cluster_t clus = dir->DIR_FstClusLO;
    printf("file name is %s\n", dir->DIR_Name);
    while (is_cluster_inuse(clus)) {
        printf("reading cluster %u\n", clus);
        for (int i = 0; i < meta.sec_per_clus; i++) {
            sector_t sec = cluster_first_sector(clus) + i;
            sector_read(sec, sec_buffer);
            for (size_t off = 0; off < meta.sector_size; off += sizeof(DIR_ENTRY)) {
                DIR_ENTRY* cur_dir = (DIR_ENTRY*)(sec_buffer + off);
                if (is_dot(cur_dir)) {
                    printf("find . or ..\n");
                    continue;
                }
                if (!is_free(cur_dir) && !is_deleted(cur_dir)) {
                    printf("this dir is not empty, find file name %s\n", cur_dir->DIR_Name);
                    return -ENOTEMPTY;
                }
            }
        }
        clus = read_fat_entry(clus);
    }

    printf("this dir is empty\n");

    free_clusters(dir->DIR_FstClusLO);
    dir->DIR_Name[0] = NAME_DELETED;
    dir_entry_write(slot);

    return 0;
}


// ------------------TASK3: 写文件、裁剪文件-----------------------------------


/**
 * @brief 将data中的数据写入编号为clusterN的簇的offset位置。
 *        注意size+offset <= 簇大小
 * 
 * @param fat16_ins 文件系统指针
 * @param clusterN  要写入数据的块号
 * @param data      要写入的数据
 * @param size      要写入数据的大小（字节）
 * @param offset    要写入簇的偏移量
 * @return ssize_t  成功写入的字节数，失败返回错误代码负值。可能部分成功，此时仅返回成功写入的字节数，不提供错误原因（POSIX标准）。
 */
ssize_t write_to_cluster_at_offset(cluster_t clus, off_t offset, const char* data, size_t size) {
    printf("in write_to_cluster_at_offset(clus= %u, offset= %lu, size= %lu)\n",
           clus, offset, size);
    assert(offset + size <= meta.cluster_size);  // offset + size 必须小于簇大小
    char sector_buffer[PHYSICAL_SECTOR_SIZE];

    size_t pos = 0;
    // TODO: 参考注释，以及read_from_cluster_at_offset函数，实现写入簇的功能。
    uint32_t sec = cluster_first_sector(clus) + offset / meta.sector_size;
    size_t sec_off = offset % meta.sector_size;
    while (pos < size) {
        int ret = sector_read(sec, sector_buffer);
        if (ret < 0) {
            return ret;
        }
        size_t len = min(meta.sector_size - sec_off, size - pos);
        memcpy(sector_buffer + sec_off, data + pos, len);
        sector_write(sec, sector_buffer);
        pos += len;
        sec_off = 0;
        sec++;
    }
    return pos;
}

/**
 * @brief 为文件分配新的簇至足够容纳size大小
 * 
 * @param dir 文件的目录项
 * @param size 所需的大小
 * @return int 成功返回0
 */
int file_reserve_clusters(DIR_ENTRY* dir, size_t size) {
    // TODO: 为文件分配新的簇至足够容纳size大小
    //   1. 计算需要多少簇
    //   2. 如果文件没有簇，直接分配足够的簇
    //   3. 如果文件已有簇，找到最后一个簇（哪个簇是当前该文件的最后一个簇？），并计算需要额外分配多少个簇
    //   4. 分配额外的簇，并将分配好的簇连在最后一个簇后

    printf("in file_reserve_clusters, extend size is %lu, old file size= %u\n",
           size, dir->DIR_FileSize);
    cluster_t new_cluster_num = 0;
    cluster_t first_cluster_index;
    if (dir->DIR_FileSize == 0) {
        new_cluster_num = size / meta.cluster_size + 1;
        printf("current file does not have cluster, need %u clusters\n",
               new_cluster_num);
        int ret = alloc_clusters(new_cluster_num, &first_cluster_index);
        printf("ret from alloc_clusters is %d\n", ret);
        dir->DIR_FstClusLO = first_cluster_index;
        printf("out file_reserve_clusters\n");
        return 0;
    }
    //当前文件没有簇
    else {
        size_t space_in_last_cluster = 0;
        if (dir->DIR_FileSize % meta.cluster_size == 0) {
            space_in_last_cluster = 0;
        } else {
            space_in_last_cluster =
                meta.cluster_size - dir->DIR_FileSize % meta.cluster_size;
        }
        if (space_in_last_cluster >= size) {
            printf("remains is last cluster is %lu, don't need alloc new "
                   "cluster\n",
                   space_in_last_cluster);
            printf("out file_reserve_clusters\n");
            return 0;
        } else {
            new_cluster_num =
                (size - space_in_last_cluster) / meta.cluster_size + 1;
        }
        printf("current file have cluster, need %u clusters\n",
               new_cluster_num);

        int ret = alloc_clusters(new_cluster_num, &first_cluster_index);
        printf("ret from alloc_clusters is %d\n", ret);

        cluster_t last_cluster_index;
        last_cluster_index = dir->DIR_FstClusLO;
        while (true) {
            cluster_t next = read_fat_entry(last_cluster_index);
            if (is_cluster_end(next)) {
                break;
            }
            last_cluster_index = next;
        }

        write_fat_entry(last_cluster_index, first_cluster_index);
        printf("out file_reserve_clusters\n");
        return 0;
    }
    //当前文件已有簇
}



/**
 * @brief 将长度为size的数据data写入path对应的文件的offset位置。注意当写入数据量超过文件本身大小时，
 *        需要扩展文件的大小，必要时需要分配新的簇。
 * 
 * @param path    要写入的文件的路径
 * @param data    要写入的数据
 * @param size    要写入数据的长度
 * @param offset  文件中要写入数据的偏移量（字节）
 * @param fi      本次实验可忽略该参数
 * @return int    成功返回写入的字节数，失败返回POSIX错误代码的负值。
 */
int fat16_write(const char *path, const char *data, size_t size, off_t offset,
                struct fuse_file_info *fi) {
    printf("write(path='%s', offset=%ld, size=%lu)\n", path, offset, size);
    // TODO: 写文件，请自行实现，将在下周发布进一步说明。
    
    DirEntrySlot slot;
    DIR_ENTRY *dir;
    find_entry(path, &slot);
    dir = &(slot.dir);

    size_t start=offset;
    size_t end=offset+size;
    printf("start is %lu, end is %lu\n",start,end);

    if (end > slot.dir.DIR_FileSize) {
        printf("need extend file size\n");
        file_reserve_clusters(dir, end - dir->DIR_FileSize);
    }

    cluster_t clus = dir->DIR_FstClusLO;
    size_t pointer = 0;
    while (true) {
        if (pointer <= start && pointer + meta.cluster_size > start) {
            break;
        }
        pointer += meta.cluster_size;
        clus = read_fat_entry(clus);
    }

    size_t cur_size;
    size_t incr;
    size_t p = 0;
    cur_size = min(size, meta.cluster_size - (start - pointer));
    incr = write_to_cluster_at_offset(clus, start - pointer, data, cur_size);
    p += incr;
    data += incr;
    clus = read_fat_entry(clus);
    printf("incr= %lu, p= %lu, pointer= %lu\n", incr, p, pointer);
    pointer += meta.cluster_size;
    while (p < size) {
        if (pointer + meta.cluster_size < end) {
            printf("end is not in current cluster\n");
            cur_size = meta.cluster_size;
            p += write_to_cluster_at_offset(clus, 0, data, cur_size);
            data += cur_size;
            pointer += cur_size;
            clus = read_fat_entry(clus);
        } else {
            printf("end is in current cluster\n");
            cur_size = size - p;
            p += write_to_cluster_at_offset(clus, 0, data, cur_size);
            pointer += cur_size;
            data += cur_size;
        }
    }

    if (end > dir->DIR_FileSize) {
        dir->DIR_FileSize += p;
    }
    dir_entry_write(slot);
    return p;
}

/**
 * @brief 将path对应的文件大小改为size，注意size可以大于小于或等于原文件大小。
 *        若size大于原文件大小，需要将拓展的部分全部置为0，如有需要，需要分配新簇。
 *        若size小于原文件大小，将从末尾截断文件，若有簇不再被使用，应该释放对应的簇。
 *        若size等于原文件大小，什么都不需要做。
 * 
 * @param path 需要更改大小的文件路径 
 * @param size 新的文件大小
 * @return int 成功返回0，失败返回POSIX错误代码的负值。
 */
int fat16_truncate(const char *path, off_t size, struct fuse_file_info* fi) {
    printf("truncate(path='%s', size=%lu)\n", path, size);
    // TODO：裁剪文件，请自行实现，将在下周发布说明。

    DirEntrySlot slot;
    DIR_ENTRY *dir = &(slot.dir);
    find_entry(path, &slot);

    size_t old_size = dir->DIR_FileSize;
    printf("old size is %lu\n", old_size);
    if (size == old_size) {
        printf("new size equals old size, out fat16_truncate\n");
        return 0;
    }
    if (size > old_size) {
        size_t size_diff = size - old_size;
        file_reserve_clusters(dir, size_diff);
        dir->DIR_FileSize = size;
        dir_entry_write(slot);
        printf("new size is larger than old_size, out fat16_truncate\n");
        return 0;
    }
    if (size < old_size) {
        cluster_t clus = dir->DIR_FstClusLO;
        size_t pointer = 0;
        while (true) {
            if (pointer <= size && size < pointer + meta.cluster_size) {
                break;
            }
            clus = read_fat_entry(clus);
            pointer += meta.cluster_size;
        }

        char ZERO[PHYSICAL_SECTOR_SIZE]={0};
        char sector_buffer[PHYSICAL_SECTOR_SIZE];
        sector_t first_sec = cluster_first_sector(clus);
        sector_t sec=first_sec;
        while (true) {
            if (pointer <= size && size < pointer + meta.sector_size) {
                break;
            }
            sec++;
            pointer += meta.sector_size;
        }
        sector_read(sec, sector_buffer);
        memcpy(sector_buffer+size%meta.sector_size, ZERO, meta.sector_size-(size%meta.sector_size));
        sector_write(sec, sector_buffer);
        sec++;
        while (sec<first_sec+meta.sec_per_clus) {
            sector_write(sec, ZERO);
            sec++;
        }

        cluster_t next_clus=read_fat_entry(clus);
        free_clusters(next_clus);
        write_fat_entry(clus, CLUSTER_END);
        
        dir->DIR_FileSize=size;
        dir_entry_write(slot);
        printf("new size is less than old size, out fat16_truncate\n");
        return 0;
    }
    return 0;
}


struct fuse_operations fat16_oper = {
    .init = fat16_init,
    .destroy = fat16_destroy,
    .getattr = fat16_getattr,

    // TASK1: tree [dir] / ls [dir] ; cat [file] / tail [file] / head [file]
    .readdir = fat16_readdir,
    .read = fat16_read,

    // TASK2: touch [file]; rm [file]
    .mknod = fat16_mknod,
    .unlink = fat16_unlink,
    .utimens = fat16_utimens,

    // TASK3: mkdir [dir] ; rm -r [dir]
    .mkdir = fat16_mkdir,
    .rmdir = fat16_rmdir,

    // TASK4: echo "hello world!" > [file] ;  echo "hello world!" >> [file]
    .write = fat16_write,
    .truncate = fat16_truncate
};
