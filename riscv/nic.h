#ifndef _RISCV_NIC_H
#define _RISCV_NIC_H

#include "decode.h"

class nic_t {
 public:
	nic_t();
	reg_t load_uint64();
	void store_uint64(reg_t data);
 private:
 	uint64_t hidden_data = 0;
};

#endif