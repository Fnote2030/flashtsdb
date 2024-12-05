#ifndef _FLASHTSDB_H_
#define _FLASHTSDB_H_

//#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif


#define FDB_SECTOR_SIZE (4096)



#pragma pack(1)
typedef struct _tsdb_flash_handle
{
    uint32_t start_addr;        // 分配的FLASH区域的起始地址
    uint32_t db_size;           // 分配的flash的空间大小
    uint16_t unit_size;         // 一个数据单元的大小
    
    uint16_t min_sec;           // 最早点对应的扇区偏移
    uint16_t max_sec;           // 最新点对应的扇区偏移
    uint16_t unit_num;          // max_sec上的数据单元使用了几个
    uint16_t crc16;             // 所有数据成员的校验
} tsdb_flash_handle;
 

// 备份fdb管理结构 20个字节 
typedef struct _tsdb_flash_handle_bk
{
    uint8_t type;  // 1为该结构
    // 0xF1 准备更新(上电后需要将备份恢复到节点对应fdb管理结构存储区域)，0x01-更新成功(不需要处理)
    uint8_t status; 
    tsdb_flash_handle flash_handle;
} tsdb_flash_handle_bk;

// 中转一个扇区的数据单元 7个字节
typedef struct _tsdb_unit_repair_bk
{
    uint8_t type;  // 2为该结构
    uint16_t sec_idx;
    uint16_t data_size;
    uint16_t crc16;
} tsdb_unit_repair_bk;
#pragma pack() 


typedef struct fdb_tsdb
{
    tsdb_flash_handle handle;
    uint16_t offset;        // 目前最新的fdb管理结构放在第几个单元
    uint16_t sec_size;    
    uint32_t oldest_ts;     // 最早点的时间戳
    uint32_t newest_ts;     // 最新点的时间戳
    uint8_t ts_offset;      // 该成员值为0，unit 的结构为 4字节时间戳 + 2字节crc校验 + 数据
    uint8_t crc_offset;     // 该成员值为4，unit 的结构为 4字节时间戳 + 2字节crc校验 + 数据
    uint8_t data_offset;    // 该成员值为6，unit 的结构为 4字节时间戳 + 2字节crc校验 + 数据
    uint8_t init_flg;       // 0x5A：已经初始化，其他值：未初始化
}*fdb_tsdb_t;



typedef struct fdb_tsl 
{
    uint32_t addr;
    uint16_t unit_size;
    uint32_t time;
    uint32_t crc16_addr;
    uint32_t data_addr;
    uint16_t data_size;
}*fdb_tsl_t;


typedef struct _fdb_unit_location
{
    uint16_t sec_idx; 
    uint16_t unit_idx; 
} fdb_unit_location;

typedef bool (*fdb_tsl_cb)(fdb_tsl_t tsl, void *arg);





bool fdb_tsdb_init(fdb_tsdb_t db, uint32_t addr, uint32_t size, uint16_t data_size);
bool fdb_tsdb_deinit(fdb_tsdb_t db);

bool fdb_tsl_store(fdb_tsdb_t db, void *pin, uint16_t size, uint32_t timestamp);
bool fdb_query_tsl_by_index(fdb_tsdb_t db, uint32_t index, fdb_tsl_cb cb, void *cb_arg);
uint32_t fdb_query_tsl_by_time(fdb_tsdb_t db, uint32_t from, uint32_t to, fdb_tsl_cb cb, void *cb_arg);
uint32_t fdb_query_tsl_num(fdb_tsdb_t db);

uint32_t fdb_read_data_by_location(fdb_tsdb_t db, fdb_unit_location *plocation, void *pout, uint32_t outlen);
uint16_t fdb_read_tsl(fdb_tsl_t tsl, void *pout, uint16_t outlen, uint8_t mode);


#ifdef __cplusplus
}
#endif

#endif 
