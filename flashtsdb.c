#include <flashtsdb.h>



#ifndef FDB_ASSERT
#define FDB_ASSERT(EXPR)                                                      \
if (!(EXPR))                                                                  \
{                                                                             \
    FDB_PRINT("(%s) has assert failed at %s.\n", #EXPR, __func__);            \
    while (1);                                                                \
}
#endif



#define FDB_PRINT(...)      ((void)0)

#define FDB_DB_IS_INIT(db)  (((fdb_tsdb_t)db)->init_flg == 0x5A)
#define FDB_DB_IS_EMPTY(db)  (((fdb_tsdb_t)db)->handle.max_sec == ((fdb_tsdb_t)db)->handle.min_sec && ((fdb_tsdb_t)db)->handle.unit_num == 0)

#define FDB_OFFSETOF(type, member) ((uint32_t)&(((type *)0)->member))



static uint32_t _fdb_flash_byte_read(uint8_t *pout, uint32_t addr, uint32_t len)
{
    return len;
}

static uint32_t _fdb_flash_byte_write(uint8_t *pin, uint32_t addr, uint32_t len)
{
    return len;
}

static bool _fdb_flash_sector_erase(uint32_t addr)
{
    addr = (addr / FDB_SECTOR_SIZE) * FDB_SECTOR_SIZE;
    return true;
}
static uint16_t _fdb_crc16_exp(uint8_t *pin, uint16_t len, uint16_t crc16)
{
    return 0;
}



static uint32_t _fdb_flash_byte_copy(uint32_t src_addr, uint32_t dest_addr, uint32_t len)
{
    uint8_t data[100];
    uint16_t data_size;
    uint16_t read_write_size;
    
    read_write_size = data_size = sizeof(data);
    _fdb_flash_sector_erase(dest_addr);
    for(uint32_t i=0; i<len; i+=data_size)
    {
        if(i+data_size > len)
        {
            read_write_size = len - i;
        }
        _fdb_flash_byte_read(data, src_addr+i, read_write_size);
        if(((dest_addr+i) % FDB_SECTOR_SIZE) == 0)
        {
            _fdb_flash_sector_erase(dest_addr+i);
        }
        _fdb_flash_byte_write(data, dest_addr+i, read_write_size);
    }
    return len;
}
// 读取一定长度的flash数据并检查是否都为ch
static bool _fdb_flash_byte_cmp(uint32_t src_addr, uint8_t ch, uint32_t len)
{
    uint8_t data[100];
    uint16_t data_size;
    uint16_t read_size;
    
    read_size = data_size = sizeof(data);
    for(uint32_t i=0; i<len; i+=data_size)
    {
        if(i+data_size > len)
        {
            read_size = len - i;
        }
        _fdb_flash_byte_read(data, src_addr+i, read_size);

        for(int idx=0; idx<read_size; idx++)
        {
            if(data[idx] != ch)
                return false;
        }
    }
    return true;
}


static bool _get_db_next_location(fdb_tsdb_t db, fdb_unit_location *plocation)
{    
    uint16_t unit_sum_1sec, db_sec_sum;
    bool ret = true;

    if(db == NULL || !FDB_DB_IS_INIT(db) || FDB_DB_IS_EMPTY(db) || plocation == NULL)
        return false;    

    unit_sum_1sec = db->sec_size / db->handle.unit_size;
    db_sec_sum = db->handle.db_size / db->sec_size - 2;
    // 遍历下一个时序记录
    if(plocation->unit_idx >= (unit_sum_1sec-1))
    {
        if(((plocation->sec_idx + 1) % db_sec_sum) > db->handle.max_sec)
        {
            ret = false;
        }
        else
        {
            plocation->sec_idx = (plocation->sec_idx + 1) % db_sec_sum;
            plocation->unit_idx = 0;
        }
    }
    else
    {
        if(plocation->sec_idx == db->handle.max_sec && plocation->unit_idx+1 >= db->handle.unit_num)
        {
            ret = false;
        }
        else
        {
            plocation->unit_idx++;
        }
    }

    return ret;
}


/**************************************************
函数名:
功能：数据单元修复
入参：phandle_bk  -- 要记录的fdb管理结构
出参：none
***************************************************/
static void _set_db_unit_repair_bk(fdb_tsdb_t db, tsdb_unit_repair_bk *punit_repair, uint32_t src_addr)
{
    uint32_t bk_addr = 0;
    uint32_t offset = 0;

    FDB_ASSERT(db != NULL);
    FDB_ASSERT(punit_repair != NULL);
    FDB_ASSERT((src_addr % db->sec_size) == 0);
    FDB_ASSERT(src_addr >= db->handle.start_addr + 2 * db->sec_size);
    FDB_ASSERT(src_addr < db->handle.start_addr + db->handle.db_size);
    FDB_ASSERT(sizeof(tsdb_unit_repair_bk) + punit_repair->data_size <= db->sec_size);

    bk_addr = db->handle.start_addr + db->sec_size;

    _fdb_flash_byte_copy(src_addr, bk_addr + sizeof(tsdb_unit_repair_bk), punit_repair->data_size);   
    
    punit_repair->type = 2;
    punit_repair->crc16 = _fdb_crc16_exp((uint8_t *)punit_repair, sizeof(tsdb_unit_repair_bk)-sizeof(punit_repair->crc16), 0xFFFF);
    _fdb_flash_byte_write((uint8_t *)(punit_repair), bk_addr, sizeof(tsdb_unit_repair_bk));
    
    _fdb_flash_byte_copy(bk_addr + sizeof(tsdb_unit_repair_bk), src_addr, punit_repair->data_size);

    //只写一个字节
    punit_repair->crc16 = 0;
    offset = FDB_OFFSETOF(tsdb_unit_repair_bk, crc16);
    _fdb_flash_byte_write((uint8_t *)(&punit_repair->crc16), bk_addr + offset, sizeof(punit_repair->crc16));
}



/**************************************************
函数名:
功能：记录fdb管理结构备份的状态
入参：phandle_bk  -- 要记录的fdb管理结构
        phandle_bk->status == 0xF1 -- 准备更新fdb管理结构
        phandle_bk->status == 0x01 -- 更新fdb管理结构成功(备份无效化)

      注意:先传0xF1，再传0x01，直接传0x01不会有任何效果
出参：none
***************************************************/
static void _set_db_flash_handle_bk(fdb_tsdb_t db, tsdb_flash_handle_bk *phandle_bk)
{
    uint32_t handle_bk_add = 0;
    uint32_t offset = 0;
    static uint8_t have_record_start_update_flag = 0; // 是否提前调用了一次该接口记录准备更新fdb管理结构(phandle_bk->status == 0xF1)

    FDB_ASSERT(db != NULL);
    FDB_ASSERT(phandle_bk != NULL);
    
    handle_bk_add = db->handle.start_addr + db->sec_size;

    // 更新fdb管理结构成功，无效化该备份
    if (phandle_bk->status == 0x01)
    {
        if (have_record_start_update_flag == 1)
        {
            have_record_start_update_flag = 0;
            //只写一个字节
            offset = ((uint32_t)(&phandle_bk->status) - (uint32_t)(phandle_bk));
            _fdb_flash_byte_write((uint8_t *)(&phandle_bk->status), handle_bk_add + offset, sizeof(phandle_bk->status));

            return;
        }
        else // 前面没传phandle_bk->status == 0xF1参数调用，没找到写入的fdb管理结构备份地址
        {
            return;
        }
    }
    else if(phandle_bk->status == 0xF1)
    {
        _fdb_flash_sector_erase(handle_bk_add);
        _fdb_flash_byte_write((uint8_t *)(phandle_bk), handle_bk_add, sizeof(tsdb_flash_handle_bk));

        have_record_start_update_flag = 1;
    }
}


static bool _set_db_flash_handle(fdb_tsdb_t db)
{
    uint32_t  data_addr = 0, handle_addr = 0;
    uint16_t handle_size = sizeof(db->handle);
    uint16_t handle_sum_1sec = db->sec_size / handle_size;
    uint16_t db_sec_sum = db->handle.db_size / db->sec_size - 2; // 第0个sec用来存储handle，第1个用来中转备份
    uint16_t unit_sum_1sec = db->sec_size / db->handle.unit_size;
    tsdb_flash_handle all_0xff_buf, temp_handle;

    data_addr = db->handle.start_addr + 2 * db->sec_size;
	data_addr = data_addr;
    
    if (!((db->handle.unit_num <= unit_sum_1sec) 
        && (db->handle.max_sec < db_sec_sum) 
        && (db->handle.min_sec < db_sec_sum)) 
        )
    {
        FDB_PRINT("%s : flash_handle is illegal!!!\n", __func__);
        return false;
    }
    memset(&all_0xff_buf, 0xff, handle_size);


    // 1.更新此次要写入的位置
    if(db->offset >= (handle_sum_1sec-1))
    {
        db->offset = 0;
        handle_addr = db->handle.start_addr;
    }
    else
    {
        db->offset += 1;
        handle_addr = db->handle.start_addr + db->offset * handle_size;

        _fdb_flash_byte_read((uint8_t *)(&temp_handle), handle_addr, handle_size);

        if (memcmp(&temp_handle, &all_0xff_buf, handle_size) != 0) 
        {
            // 不是全为0xff，则说明该单元不能直接写入
            db->offset = 0;
            handle_addr = db->handle.start_addr;
        }
    }
    
    /* 2. 更新handle的crc */
    db->handle.crc16 = _fdb_crc16_exp((uint8_t *)(&db->handle), FDB_OFFSETOF(tsdb_flash_handle, crc16), 0xFFFF);
    
    /* 3.根据位置分类处理写入 */
    // 3.1 需要先执行扇区擦除
    if(db->offset == 0)
    {
        tsdb_flash_handle_bk handle_bk;
        // 记录准备更新(备份)
        handle_bk.type = 1;
        handle_bk.status = 0xF1;
        memcpy(&(handle_bk.flash_handle), (uint8_t *)(&db->handle), handle_size);
        
        _set_db_flash_handle_bk(db, &handle_bk);
        
        // 开始更新fdb管理结构
        _fdb_flash_sector_erase(handle_addr);
        _fdb_flash_byte_write((uint8_t *)(&db->handle), handle_addr, handle_size);
        
        // 记录更新成功(无效化备份)
        handle_bk.status = 0x01;
        _set_db_flash_handle_bk(db, &handle_bk);
    }
    // 3.2 可以直接写入
    else
    {
        _fdb_flash_byte_write((uint8_t *)(&db->handle), handle_addr, handle_size);
    }

    return true;
}



/**************************************************
函数名:
功能：获取某个db对应的fdb管理结构的最新值
      (一页flash分成多个fdb管理结构，最后一个单元存放的为目前的实际值)
入参：
出参：0： 没有fdb管理结构，1：有fdb管理结构  ， 2: 配置改变，新建数据库
***************************************************/
static uint8_t _get_db_flash_handle(fdb_tsdb_t db)
{
    uint32_t handle_addr = 0;
    int16_t handle_index = 0;
    struct fdb_tsdb tmp_db;
    uint16_t crc16 = 0xFFFF;
    uint8_t handle_err_cnt = 0, find_flg = 0x00;
    uint16_t handle_size = sizeof(tsdb_flash_handle);
    uint16_t handle_sum_1sec = db->sec_size / handle_size;
    tsdb_flash_handle tmp_handle;

    handle_addr = db->handle.start_addr; 

    memset((char *)&tmp_handle, 0xff, handle_size);
        
    for (handle_index = handle_sum_1sec - 1; handle_index >= 0; handle_index--) // 从后面往前遍历
    {
        _fdb_flash_byte_read((uint8_t *)(&tmp_db.handle), handle_addr + handle_index*handle_size, handle_size);
        if (memcmp((char *)&tmp_db.handle, (char *)&tmp_handle, handle_size) != 0)
        {
            // 不是全为0xff，则说明该单元可能为最新的fdb管理结构            
            crc16 = _fdb_crc16_exp((uint8_t *)(&tmp_db.handle), FDB_OFFSETOF(tsdb_flash_handle, crc16), crc16);
            if (tmp_db.handle.crc16 == crc16)
            {
                db->offset = handle_index;
                find_flg = 0x01;
                break;
            }
            else
            {
                handle_err_cnt++; // 容错
            }
        }
    }
    /* 2.判断是否找到了 */
    if (find_flg == 0x00) 
    {
        db->offset = 0xFFFF;
        _set_db_flash_handle(db);
        return 0;
    }

    /* 3.判断配置是否改变，改变则覆盖旧的数据库 */
    if((db->handle.start_addr != tmp_db.handle.start_addr)
        || (db->handle.db_size != tmp_db.handle.db_size)
        || (db->handle.unit_size != tmp_db.handle.unit_size))
    {
        db->offset = 0xFFFF;
        _set_db_flash_handle(db);
        return 2;
    }
    memcpy(&db->handle, &tmp_db.handle, handle_size);
    
    /*
        4. 容错, 后面有fdb管理结构出错，找到的这个fdb管理结构不是最新的fdb管理结构
        写入的时候刚好掉电才会产生这种错误，handle_err_cnt理论上最大只能到1
    */
    if (handle_err_cnt == 1)
    {
        // 4.1 更新flash_handle
        uint16_t db_sec_sum = db->handle.db_size / db->sec_size - 2; // 第0个sec用来存储handle，第1个用来中转备份
        uint16_t unit_sum_1sec = db->sec_size / db->handle.unit_size;

        if(tmp_db.handle.unit_num < unit_sum_1sec)
        {
            tmp_db.handle.unit_num++; // 当前扇区还没用完
        }
        else
        {
            if(tmp_db.handle.min_sec == ((tmp_db.handle.max_sec + 1) % db_sec_sum)) 
            {
                tmp_db.handle.min_sec = (tmp_db.handle.min_sec + 1) % db_sec_sum;// 滚动覆盖一个扇区的空间
            }
            tmp_db.handle.max_sec = (tmp_db.handle.max_sec + 1) % db_sec_sum; 
            tmp_db.handle.unit_num = 1;
        }

        // 4.2 检查新位置是否存储了校验通过的时序记录
        fdb_unit_location location;
        location.sec_idx = tmp_db.handle.max_sec;
        location.unit_idx = tmp_db.handle.unit_num - 1;
        
        memcpy(&tmp_handle, &tmp_db.handle, sizeof(tmp_handle));
        memcpy(&tmp_db, db, sizeof(tmp_db));
        memcpy(&tmp_db.handle, &tmp_handle, sizeof(tmp_db.handle));
        if(fdb_read_data_by_location(&tmp_db, &location, NULL, 0))
        {
            // 更新到flash
            memcpy(&db->handle, &tmp_db.handle, handle_size);
            _set_db_flash_handle(db);
        }  
    }
    else if (handle_err_cnt > 1)
    {
        FDB_PRINT("\n--------- err [%s, %s, line = %d], (handle_err_cnt > 1) ---------\n", __FILE__, __func__, __LINE__);
    }

    return 1;
}


static bool _fdb_tsdb_check_data(fdb_tsdb_t db)
{
    uint32_t sec_addr = 0, data_addr = 0;
    uint16_t unit_sum_1sec, db_sec_sum;
    uint16_t remain_len = 0;
    
    if(db == NULL)
    {
        return false;
    }
    
    sec_addr = db->handle.start_addr + db->sec_size * (2 + db->handle.max_sec);
    db_sec_sum = db->handle.db_size / db->sec_size - 2;
    unit_sum_1sec = db->sec_size / db->handle.unit_size;
    if(db->handle.unit_num != unit_sum_1sec)
    {
        // 需要该扇区剩余空间是否为全0xFF
        data_addr = sec_addr + db->handle.unit_num * db->handle.unit_size;
        remain_len = db->sec_size - db->handle.unit_num * db->handle.unit_size;
        if(!_fdb_flash_byte_cmp(data_addr, 0xFF, remain_len))
        {
            tsdb_unit_repair_bk unit_repair;
            unit_repair.sec_idx = db->handle.max_sec;
            unit_repair.data_size = db->handle.unit_num * db->handle.unit_size;
            _set_db_unit_repair_bk(db, &unit_repair, sec_addr);
        }
    }
    else if(((db->handle.max_sec + 1) % db_sec_sum) == db->handle.min_sec)
    {
        // 自修复机制2
        uint8_t update_flg = 1;
        fdb_unit_location location;
        uint32_t ts1, ts2;
        location.unit_idx = 0;
        location.sec_idx = db->handle.min_sec;
        if(fdb_read_data_by_location(db, &location, (uint8_t *)(&ts1), sizeof(ts1)))
        {
            if(_get_db_next_location(db, &location))
            {
                if(fdb_read_data_by_location(db, &location, (uint8_t *)(&ts2), sizeof(ts2)))
                {
                    if(ts2 > ts1) // 1个扇区只存储一条时序记录，需要该操作
                        update_flg = 0;
                }
                // else: 可能是min_sec扇区写了一个最新的时序记录
            }
            else
            {
                update_flg = 0;
            }
        }
        if(update_flg)
        {
            // 更新到flash
            db->handle.min_sec = (db->handle.min_sec + 1) % db_sec_sum;
            _set_db_flash_handle(db);
        }
    }
    
    if(!FDB_DB_IS_EMPTY(db))
    {    
        data_addr = sec_addr + (db->handle.unit_num-1)*db->handle.unit_size;
        _fdb_flash_byte_read((uint8_t *)&db->newest_ts, data_addr, sizeof(db->newest_ts));
        
	    _fdb_flash_byte_read((uint8_t *)&db->oldest_ts, db->handle.start_addr + (2 + db->handle.min_sec)*db->sec_size, sizeof(db->oldest_ts));
    }
	return true;
}



/**************************************************
函数名:
功能：检查db是否需要执行修复操作
入参：
出参：
***************************************************/
static bool _fdb_tsdb_repair(fdb_tsdb_t db)
{
    uint32_t bk_addr = 0, data_addr = 0, sec_addr = 0;
    uint16_t offset = 0;
    uint8_t tmp_data[100];
    uint16_t tmp_crc16;
    tsdb_flash_handle_bk *phandle_bk;
	tsdb_unit_repair_bk *unit_repair_bk;
	
    if(db == NULL)
    {
        return false;
    }
    
    bk_addr = db->handle.start_addr + db->sec_size;
    
    _fdb_flash_byte_read(tmp_data, bk_addr, sizeof(tmp_data));
    switch(tmp_data[0])
    {
        case 1: // 修复flash_handle
            phandle_bk = (tsdb_flash_handle_bk *)tmp_data;
            if(phandle_bk->status == 0xF1)
            {
                tmp_crc16 = _fdb_crc16_exp((uint8_t *)(&phandle_bk->flash_handle), FDB_OFFSETOF(tsdb_flash_handle, crc16), 0xFFFF);
                if(tmp_crc16 == phandle_bk->flash_handle.crc16)
                {
                    /* 判断配置是否改变，改变则新建数据库覆盖旧的数据库 */
                    if((db->handle.start_addr != phandle_bk->flash_handle.start_addr)
                        || (db->handle.db_size != phandle_bk->flash_handle.db_size)
                        || (db->handle.unit_size != phandle_bk->flash_handle.unit_size))
                    {
                        db->offset = 0xFFFF;
                        _set_db_flash_handle(db);
                    }
                    else
                    {
                        memcpy(&db->handle, &phandle_bk->flash_handle, sizeof(db->handle));
                        _set_db_flash_handle(db);
                    }
                    offset = FDB_OFFSETOF(tsdb_flash_handle_bk, status);
                    phandle_bk->status = 0x01;
                    _fdb_flash_byte_write((uint8_t *)(&phandle_bk->status), bk_addr + offset, sizeof(phandle_bk->status));
                }
            }
            break;
            
        case 2:
            unit_repair_bk = (tsdb_unit_repair_bk *)tmp_data;
            tmp_crc16 = _fdb_crc16_exp((uint8_t *)unit_repair_bk, FDB_OFFSETOF(tsdb_unit_repair_bk, crc16), 0xFFFF);
            if(tmp_crc16 == unit_repair_bk->crc16)
            {
                /* 判断数据库配置是否改变 */
                if((unit_repair_bk->sec_idx < (db->handle.db_size/db->sec_size - 2))
                    && (unit_repair_bk->data_size <= db->sec_size)
                    && (unit_repair_bk->data_size <= (db->sec_size - sizeof(tsdb_unit_repair_bk))))
                {
                    data_addr = bk_addr + sizeof(tsdb_unit_repair_bk);
                    sec_addr = db->handle.start_addr + (2 + unit_repair_bk->sec_idx)*db->sec_size;

                    _fdb_flash_byte_copy(data_addr, sec_addr, unit_repair_bk->data_size);
                }
                unit_repair_bk->crc16 = 0;
                offset = FDB_OFFSETOF(tsdb_unit_repair_bk, crc16);
                _fdb_flash_byte_write((uint8_t *)(&unit_repair_bk->crc16), bk_addr + offset, sizeof(unit_repair_bk->crc16));
            }
            break;
            
        default:
            break;
    }
    
    return true;
}


/**************************************************
函数名:
功能：给出左边和右边时序记录位置，然后返回中间的时序记录位置
入参： 
       注意：左右位置之间的时序记录为偶数时（包括左右位置上只有一个时序记录，总共两个的时候），返回偏向左边的时序记录
出参：  0x00: 没找到(左点和右点是同一个值)   0x01: 找到了  0x02: 左右位置不合法
***************************************************/
static uint8_t _fdb_cal_mid_unit_location(fdb_tsdb_t db, 
    fdb_unit_location *pmid, fdb_unit_location min, fdb_unit_location max)
{
    uint16_t unit_inc = 0;  
    uint16_t sec_inc = 0;             
    int32_t unit_offset_insec = 0;
    uint32_t unit_offset = 0;
    uint16_t db_sec_sum, unit_sum_1sec;

    db_sec_sum = db->handle.db_size / db->sec_size - 2;
    unit_sum_1sec = db->sec_size / db->handle.unit_size;

    if (min.sec_idx == max.sec_idx)
    {
        if (min.unit_idx == max.unit_idx)
        {
            memcpy(pmid, &min, sizeof(min));
            return 0x00;
        }
        else if ((min.unit_idx > max.unit_idx))
        {
            return 0x02; // 同一扇区的时候，不可能是这种情况
        }
    }

    if (max.sec_idx >= min.sec_idx) // 较大的时间在较小的时间的右边
    {
        unit_offset_insec = max.unit_idx - min.unit_idx;      // 可能是负数
        unit_offset = (max.sec_idx - min.sec_idx) * unit_sum_1sec + unit_offset_insec; 
    }
    else // 较大的时间在较小的时间的左边
    {
        unit_offset_insec = max.unit_idx - min.unit_idx; // 可能是负数
        unit_offset = (db_sec_sum - min.sec_idx + max.sec_idx) * unit_sum_1sec + unit_offset_insec;
    }
    unit_inc = (unit_offset / 2) % unit_sum_1sec;
    sec_inc = (unit_offset / 2) / unit_sum_1sec;

    pmid->unit_idx = (min.unit_idx + unit_inc) % unit_sum_1sec;
    pmid->sec_idx = (min.sec_idx + sec_inc) + (((min.unit_idx + unit_inc) >= unit_sum_1sec) ? 1 : 0);
    pmid->sec_idx = (pmid->sec_idx) % db_sec_sum;

    return 0x01;
}


/**************************************************
函数名:
功能：获取某个时间的数据单元位置
入参：  
        db           : 数据库
        target_time  ：需要定位的数据单元的时间
        pout         : 用于存放定位到的位置
出参：     0x00: 没找到(返回target_time后面一个点的位置)   
        0x01: 找到了 
        0x02: 返回最早点位置(target_time<最早点时间)
        0x03: 输入时间不合法(target_time>最新点时间)
        0x04: 没存储数据
***************************************************/ 
static uint8_t _fdb_find_unit_location_by_time(fdb_tsdb_t db, uint32_t target_time, fdb_unit_location *pout)
{
    uint8_t ret_ack = 0x00;
    fdb_unit_location min_location, mid_location, max_location;
    uint32_t mid_add = 0;
    uint32_t mid_time;
    uint8_t get_middot_flag = 0;
    uint32_t db_data_addr;
    uint16_t db_sec_sum, unit_sum_1sec;

    if(FDB_DB_IS_EMPTY(db))
    {
        ret_ack = 0x04;
        return ret_ack;
    }

    if(target_time <= db->newest_ts)
    {
        if(target_time < db->oldest_ts)
        {
            pout->sec_idx = db->handle.min_sec;
            pout->unit_idx = 0;
            ret_ack = 0x02;
            return ret_ack;  
        }
    }
    else
    {
        ret_ack = 0x03;
        return ret_ack;  
    }

    db_sec_sum = db->handle.db_size / db->sec_size - 2;
    unit_sum_1sec = db->sec_size / db->handle.unit_size;
    db_data_addr = db->handle.start_addr + 2 * db->sec_size;

    min_location.sec_idx = db->handle.min_sec;
    min_location.unit_idx = 0;
    max_location.sec_idx = db->handle.max_sec;
    max_location.unit_idx = db->handle.unit_num - 1;
    
    while (1)
    {
        get_middot_flag = _fdb_cal_mid_unit_location(db, &mid_location, min_location, max_location); // 找到中间的位置
        mid_add = db_data_addr + mid_location.sec_idx * db->sec_size + mid_location.unit_idx * db->handle.unit_size;
        _fdb_flash_byte_read((uint8_t *)(&mid_time), mid_add, sizeof(mid_time));
        
        if (mid_time == target_time)
        {
            memcpy((char *)pout, (char *)&mid_location, sizeof(fdb_unit_location));
            ret_ack = 0x01;
            break;
        }
        else
        {
            if (get_middot_flag == 0x00) // min和max为同一个值
            {
                if (mid_time > target_time)
                {
                    memcpy((char *)pout, (char *)&mid_location, sizeof(fdb_unit_location));
                }
                else
                {
                    // 二分法的特性，下一个数据单元的时间一定比target_time大
                    pout->unit_idx = (mid_location.unit_idx + 1) % unit_sum_1sec;
                    pout->sec_idx = (mid_location.sec_idx + ((mid_location.unit_idx >= (unit_sum_1sec - 1)) ? 1 : 0)) % db_sec_sum;
                }
                ret_ack = 0x00;
                break;
            }
            // 此时mid等于min，说明min和max之间没有其他时序记录，min比较过了就差max点没比较了
            if ((min_location.sec_idx == mid_location.sec_idx) && (min_location.unit_idx == mid_location.unit_idx))
            {
                mid_add = db_data_addr + max_location.sec_idx * db->sec_size + max_location.unit_idx * db->handle.unit_size;
                _fdb_flash_byte_read((uint8_t *)(&mid_time), mid_add, sizeof(mid_time));
                // 如果max点不等于开始点时间，则max点必定是开始点左右的值
                if (mid_time == target_time)
                {
                    memcpy((char *)pout, (char *)&max_location, sizeof(fdb_unit_location));
                    ret_ack = 0x01;
                }
                else if (mid_time > target_time)
                {
                    // 没找到，返回开始点后第一个采集并存储的时序记录位置
                    memcpy((char *)pout, (char *)&max_location, sizeof(fdb_unit_location));
                    ret_ack = 0x00;
                }
                else
                {
                    // 理论上这里进不来
                    pout->unit_idx = (max_location.unit_idx + 1) % unit_sum_1sec;
                    pout->sec_idx = (mid_location.sec_idx + ((mid_location.unit_idx >= (unit_sum_1sec - 1)) ? 1 : 0)) % db_sec_sum;
                    ret_ack = 0x00;
                }
                break;
            }
            if (mid_time > target_time) // 取左边的区域 [min, mid-1]
            {
                max_location.unit_idx = (mid_location.unit_idx + unit_sum_1sec - 1) % unit_sum_1sec;
                max_location.sec_idx = (mid_location.sec_idx + db_sec_sum - ((mid_location.unit_idx == 0) ? 1 : 0)) % db_sec_sum;
            }
            else if (mid_time < target_time) // 取右边的区域 [mid+1, max]
            {
                min_location.unit_idx = (mid_location.unit_idx + 1) % unit_sum_1sec;
                min_location.sec_idx = (mid_location.sec_idx + ((mid_location.unit_idx >= (unit_sum_1sec - 1)) ? 1 : 0)) % db_sec_sum;
            }
        }
    }

    return ret_ack;
}



/**************************************************
函数名:
功能：读取时序记录的用户数据
入参：
    tsl         : 时序记录的信息
    pout        ：用于返回读取到的数据，可为空
    outlen      ：pout指向的缓冲区大小
    mode        : 模式，0：不校验crc，1：校验crc，
出参： 0：校验失败，其他值：用户数据的长度
***************************************************/
uint16_t fdb_read_tsl(fdb_tsl_t tsl, void *pout, uint16_t outlen, uint8_t mode)
{
    uint16_t ret = 0;
    uint16_t crc16 = 0xFFFF, store_crc16;
    uint8_t val;
    
    if((pout == NULL) || (tsl == NULL))
    {
        return ret;
    }
    
    _fdb_flash_byte_read(pout, tsl->data_addr, outlen);
    ret = outlen;

    if(1 == mode)
    {
        crc16 = _fdb_crc16_exp((uint8_t *)pout, outlen, crc16);
        for(int i = outlen; i < tsl->data_size; i++)
        {
            _fdb_flash_byte_read((uint8_t *)&val, tsl->data_addr + i, 1);
            crc16 = _fdb_crc16_exp((uint8_t *)&val, 1, crc16);
        }
        _fdb_flash_byte_read((uint8_t *)&store_crc16, tsl->crc16_addr, sizeof(store_crc16));

        if(crc16 != store_crc16)
        {
            FDB_PRINT("%s: tsl user data crc err\r\n", __func__);
            ret = 0;
        }
    }

    return ret;
}



/**************************************************
函数名:
功能：获取指定位置的时序记录
入参：
    db          : 数据库
    location    ：位置信息
    pout        ：用于返回读取到的数据，可为空
    outlen      ：pout指向的缓冲区大小
出参： 0: 校验未通过； >0: 校验通过，数据单元的大小
***************************************************/
uint32_t fdb_read_data_by_location(fdb_tsdb_t db, fdb_unit_location *plocation, void *pout, uint32_t outlen)
{
    uint32_t tmp_crc16 = 0xFFFF, read_crc16 = 0xFFFF;
    uint8_t val;
    uint32_t data_addr = 0;
    uint32_t ret = 0;
    
    if((db == NULL) || (plocation == NULL))
    {
        return false;
    }
    ret = db->handle.unit_size;
    data_addr = db->handle.start_addr + (2 + plocation->sec_idx) * db->sec_size;
    data_addr += db->handle.unit_size * plocation->unit_idx;

    if(pout != NULL && (outlen >= db->handle.unit_size))
    {   
        _fdb_flash_byte_read((uint8_t *)pout, data_addr, db->handle.unit_size);
		memcpy(&read_crc16, ((char *)pout)+db->crc_offset, sizeof(read_crc16));
        tmp_crc16 = _fdb_crc16_exp(((uint8_t *)pout) + db->data_offset, db->handle.unit_size - db->data_offset, tmp_crc16);
        
        if(tmp_crc16 == read_crc16)
        {
            return ret;
        }
    }
    else
    {
        _fdb_flash_byte_read((uint8_t *)&read_crc16, data_addr+db->crc_offset, sizeof(read_crc16));
        for(int i=db->data_offset; i<db->handle.unit_size; i++)
        {
            _fdb_flash_byte_read(&val, data_addr + i, 1);
            if(pout != NULL && i < outlen)
            {   
                ((uint8_t*)pout)[i] = val;
            }
            tmp_crc16 = _fdb_crc16_exp(&val, 1, tmp_crc16);
        }
        if(tmp_crc16 == read_crc16)
        {
            return ret;
        }
    }

    return 0;
}



/**************************************************
函数名:
功能：初始化数据库
入参：
    db:         数据库句柄
    addr：       数据库存储的起始地址，必须对齐扇区地址
    size：       数据库的大小，扇区0用来存储管理结构，扇区1用于数据备份中转，扇区2之后才是用于存储时序记录
    data_size:  用户数据的最大长度, 大小范围为[sizeof(tsdb_unit_repair_bk)-6, FDB_SECTOR_SIZE-6]；
                    数据单元大小=用户数据的最大长度+6，一个数据单元存储一条时序记录；
                    每个时序记录包括4个字节时间戳+2字节crc16+N字节用户数据，N<=data_size
出参：
***************************************************/
bool fdb_tsdb_init(fdb_tsdb_t db, uint32_t addr, uint32_t size, uint16_t data_size)
{
    FDB_ASSERT(db != NULL);
    FDB_ASSERT((addr % FDB_SECTOR_SIZE) == 0);
    FDB_ASSERT((size % FDB_SECTOR_SIZE) == 0);
    FDB_ASSERT(size >= 4 * FDB_SECTOR_SIZE);
    FDB_ASSERT((data_size + 6) <= FDB_SECTOR_SIZE);
    FDB_ASSERT((data_size + 6) >= sizeof(tsdb_unit_repair_bk));

    memset(&db->handle, 0, sizeof(db->handle));
    db->offset = 0xffff;
    db->sec_size = FDB_SECTOR_SIZE;
    db->ts_offset = 0;
    db->crc_offset = sizeof(uint32_t);
    db->data_offset = sizeof(uint32_t) + sizeof(uint16_t);
    db->handle.start_addr = addr;
    db->handle.db_size = size;
    db->handle.unit_size = data_size + 6;
    
    _fdb_tsdb_repair(db);

    _get_db_flash_handle(db);

    // 检查数据单元是否可以写，不能直接写的直接修复
    _fdb_tsdb_check_data(db);
    
    db->init_flg = 0x5A;
    return true;
}

/**************************************************
函数名:
功能：重置数据库，保留原本配置，将已经存储的数据清除
入参：
    db:         数据库句柄
出参：
***************************************************/
bool fdb_tsdb_deinit(fdb_tsdb_t db)
{
    if(db == NULL)
        return false;
    if(!FDB_DB_IS_INIT(db))
        return false;

    db->handle.max_sec = 0;
    db->handle.min_sec = 0;
    db->handle.unit_num = 0;
    db->offset = 0xFFFF;
    db->newest_ts = 0;
    db->oldest_ts = 0;
    _set_db_flash_handle(db);
    
    db->init_flg = 0x5A;
    return true;
}

/**************************************************
函数名:
功能：查询存储的时序记录数量
入参：
        db:         数据库句柄
出参：
***************************************************/
uint32_t fdb_query_tsl_num(fdb_tsdb_t db)
{
    uint32_t ret = 0;
    uint32_t db_sec_sum, unit_sum_1sec;
    
    if(db == NULL || !FDB_DB_IS_INIT(db) || FDB_DB_IS_EMPTY(db))
        return ret;
        
    db_sec_sum = db->handle.db_size / db->sec_size - 2;
    unit_sum_1sec = db->sec_size / db->handle.unit_size;

    ret = unit_sum_1sec * ((db_sec_sum + db->handle.max_sec - db->handle.min_sec) % db_sec_sum) + db->handle.unit_num;
        
    return ret;
}



/**************************************************
函数名:
功能：查找数据库指定的时序记录（通过指定往前的偏移）
入参：
    db:         数据库句柄
    index:      哪个时序记录，1：最新的时序记录，2：次新的时序记录...依此类推
    cb:         用户用于处理找到的时序记录的函数指针
    cb_arg:     用户需要传递给cb的参数
出参：
***************************************************/
bool fdb_query_tsl_by_index(fdb_tsdb_t db, uint32_t index, fdb_tsl_cb cb, void *cb_arg)
{
    fdb_unit_location location;
    struct fdb_tsl tsl;
    uint32_t tsl_sum;
    uint32_t db_sec_sum, unit_sum_1sec;
    uint32_t offset = index - 1, sec_offset, unit_offset;

    if(db == NULL || !FDB_DB_IS_INIT(db) || FDB_DB_IS_EMPTY(db) || (index == 0))
        return false;

    tsl_sum = fdb_query_tsl_num(db);
    if(tsl_sum < index)
    {
        return false;
    }

    db_sec_sum = db->handle.db_size / db->sec_size - 2;
    unit_sum_1sec = db->sec_size / db->handle.unit_size;
    sec_offset = offset / unit_sum_1sec;
    unit_offset = offset % unit_sum_1sec;

    location.unit_idx = db->handle.unit_num - 1;
    if(location.unit_idx >= unit_offset)
    {
        location.unit_idx -= unit_offset;
        location.sec_idx = ((db_sec_sum - sec_offset) + db->handle.max_sec) % db_sec_sum;
    }
    else
    {
        location.unit_idx += unit_sum_1sec - unit_offset;
        location.sec_idx = ((db_sec_sum - sec_offset) + db->handle.max_sec - 1) % db_sec_sum;  
    }

    if(cb != NULL)
    {
        tsl.addr = db->handle.start_addr + (location.sec_idx + 2) * db->sec_size + location.unit_idx * db->handle.unit_size;
        tsl.unit_size = db->handle.unit_size;
        _fdb_flash_byte_read((uint8_t *)(&tsl.time), tsl.addr + db->ts_offset, sizeof(tsl.time));
        
        tsl.crc16_addr = tsl.addr + db->crc_offset;
        tsl.data_addr = tsl.addr + db->data_offset;
        tsl.data_size = db->handle.unit_size - db->data_offset;
                
        cb(&tsl, cb_arg);
    }
    return true;
}


/**************************************************
函数名:
功能：查找数据库的一个或者多个时序记录（通过指定时间段）
入参：
    db:         数据库句柄
    from:       起始时间
    to:         结束时间
    cb:         用户用于处理找到的时序记录的函数指针
    cb_arg:     用户需要传递给cb的参数
出参：
***************************************************/
uint32_t fdb_query_tsl_by_time(fdb_tsdb_t db, uint32_t from, uint32_t to, fdb_tsl_cb cb, void *cb_arg)
{
    uint32_t ret = 0;
    uint8_t status = 0x00;
    fdb_unit_location start_location;
    struct fdb_tsl tsl;

    if(db == NULL || !FDB_DB_IS_INIT(db) || FDB_DB_IS_EMPTY(db))
        return ret;
    if(from > to)
        return ret ;
    if((from > db->newest_ts) || (to < db->oldest_ts))
        return ret;

    status = _fdb_find_unit_location_by_time(db, from, &start_location);
    if(status <= 2)
    {
        tsl.unit_size = db->handle.unit_size;
        do
        {
            tsl.addr = db->handle.start_addr + (start_location.sec_idx + 2) * db->sec_size + db->handle.unit_size * start_location.unit_idx;
            _fdb_flash_byte_read((uint8_t *)&tsl.time, tsl.addr + db->ts_offset, sizeof(tsl.time));
            if(tsl.time>= from && tsl.time <= to)
            {
                if(cb!=NULL)
                {
                    tsl.crc16_addr = tsl.addr + db->crc_offset;
                    tsl.data_addr = tsl.addr + db->data_offset;
                    tsl.data_size = db->handle.unit_size - db->data_offset;
                    if(!cb(&tsl, cb_arg))
                        break;
                }
                ret++;
            }
            else
            {
                break;
            }
            // 遍历下一个时序记录
            if(!_get_db_next_location(db, &start_location))
            {
                break;
            }
        }while(1);
    }

    return ret;
}



bool fdb_tsl_store(fdb_tsdb_t db, void *pin, uint16_t size, uint32_t timestamp)
{
    uint16_t crc16 = 0xFFFF;
    uint32_t write_addr;
    uint16_t unit_sum_1sec, db_sec_sum;
    tsdb_flash_handle tmp_handle;
    
    if(db == NULL || pin == NULL || size == 0)
        return false;
    if(!FDB_DB_IS_INIT(db))
        return false;
    if(timestamp < db->newest_ts)
        return false;

    if(FDB_DB_IS_EMPTY(db))
    {
        db->oldest_ts = timestamp;
        _fdb_flash_sector_erase(db->handle.start_addr + 2 * db->sec_size);
    }
    unit_sum_1sec = db->sec_size / db->handle.unit_size;
    db_sec_sum = db->handle.db_size / db->sec_size - 2;
        
    memcpy(&tmp_handle, &db->handle, sizeof(tmp_handle));
    if(tmp_handle.unit_num >= unit_sum_1sec)
    {
        if(((tmp_handle.max_sec + 1) % db_sec_sum) == tmp_handle.min_sec)
        {
            tmp_handle.min_sec = (tmp_handle.min_sec + 1) % db_sec_sum; // 滚动覆盖一个扇区的空间
             _fdb_flash_byte_read((uint8_t *)&db->oldest_ts, db->handle.start_addr + (2 + tmp_handle.min_sec)*db->sec_size, sizeof(db->oldest_ts));
        }
        tmp_handle.max_sec = (tmp_handle.max_sec + 1) % db_sec_sum;

        write_addr = tmp_handle.start_addr + (tmp_handle.max_sec + 2) * db->sec_size;
        _fdb_flash_sector_erase(write_addr);
        tmp_handle.unit_num = 1;
    }
    else
    {
        write_addr = tmp_handle.start_addr + (tmp_handle.max_sec + 2) * db->sec_size + tmp_handle.unit_num * tmp_handle.unit_size;
        tmp_handle.unit_num = tmp_handle.unit_num + 1;
    }

    size = (size <= db->handle.unit_size - 6) ? size : (db->handle.unit_size - 6);
    crc16 = _fdb_crc16_exp((uint8_t *)pin, size, crc16);
    if(size+6 < db->handle.unit_size)
    {
        uint8_t c_0xff = 0xFF;
        for(int i=size+6; i<db->handle.unit_size; i++)
            crc16 = _fdb_crc16_exp((uint8_t *)&c_0xff, 1, crc16);
    }
    db->newest_ts = timestamp;
    _fdb_flash_byte_write((uint8_t *)pin, write_addr + db->data_offset, size);
    _fdb_flash_byte_write((uint8_t *)&db->newest_ts, write_addr + db->ts_offset, sizeof(db->newest_ts));
    _fdb_flash_byte_write((uint8_t *)&crc16, write_addr + db->crc_offset, sizeof(crc16));

    memcpy(&db->handle, &tmp_handle, sizeof(db->handle));
    _set_db_flash_handle(db);

    return true;
}



