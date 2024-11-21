#include "flashtsdb.h"


typedef struct
{
    void *out;              // 用于返回数据
    uint8_t num;            // 数组的数量
    
    uint8_t offset;
    uint8_t interval;       // 采集间隔，单位分钟
    uint32_t timestamp;     // 起始时间戳
}QUERY_FROZEN_DATA_CB_ARG_TYPE;


static struct fdb_tsdb s_forzendata_tsdb;



void fdb_store_frozen_data_init(void)
{
    fdb_tsdb_init(&s_forzendata_tsdb, 0x200000, 1024*1024, sizeof(uint32_t));
}



static bool _query_frozen_data_cb2(fdb_tsl_t tsl, void *arg)
{    
    if (arg == NULL) 
    {
        return false;
    }
    else
    { 
        _fdb_flash_byte_read((uint8_t *)arg, tsl->addr, tsl->size);
    }
	
    return true;
}


static bool _query_frozen_data_by_index(void *pdata, uint16_t index)
{
    return fdb_query_tsl_by_index(&s_forzendata_tsdb, index, _query_frozen_data_cb2, (void *)pdata);
}



static bool _query_frozen_data_cb(fdb_tsl_t tsl, void *arg)
{    
	QUERY_FROZEN_DATA_CB_ARG_TYPE *pcb_arg;

    if (arg == NULL) 
    {
        return false;
    }
    else
    {
        pcb_arg = arg;

        uint32_t start_ts = pcb_arg->timestamp;
        while(pcb_arg->offset < pcb_arg->num)
        {
            if(tsl->time == (start_ts + (pcb_arg->offset * pcb_arg->interval*60)))
            {
                _fdb_flash_byte_read((uint8_t *)pcb_arg->out, tsl->addr, tsl->size);

                pcb_arg->offset += 1;
                break;
            }
            else if(tsl->time < (start_ts + (pcb_arg->offset * pcb_arg->interval*60)))
            {
                break;
            }
            pcb_arg->offset += 1;
        }
    }
	
    return true;
}





static uint8_t _query_frozen_data_by_time(void *pdata, uint8_t num, uint32_t timestamp)
{
    uint32_t fromts, tots;
    static QUERY_FROZEN_DATA_CB_ARG_TYPE cb_arg;

    if(num == 0)
        return 0;

    fromts = timestamp;
    tots = fromts + (num - 1) * 2 * 60; 

    cb_arg.out = pdata;
    cb_arg.num = num;
    cb_arg.offset = 0;
    cb_arg.interval = 2; 
    cb_arg.timestamp = fromts;
    fdb_query_tsl_by_time(&s_forzendata_tsdb, fromts, tots, _query_frozen_data_cb, (void *)(&cb_arg));
    
    return cb_arg.offset;
}



void query_frozen_data_test(void)
{
    uint32_t out[10] = {0};

    // test 1
     _query_frozen_data_by_time(out, 10, 1729516200);
            
    // test 2
    _query_frozen_data_by_index(out, 10);

}


int main(void)
{
    fdb_store_frozen_data_init();

	query_frozen_data_test();
	return 0;
}