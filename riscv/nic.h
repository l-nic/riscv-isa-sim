#ifndef _RISCV_NIC_H
#define _RISCV_NIC_H

#include "decode.h"

class nic_t {
 public:
    nic_t();
    ~nic_t();
    reg_t read_uint64();
    void write_uint64(reg_t data);
    uint64_t num_messages_ready();
};


#endif // _RISCV_NIC_H