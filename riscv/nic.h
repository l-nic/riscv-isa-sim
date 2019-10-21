#ifndef _RISCV_NIC_H
#define _RISCV_NIC_H

#include <string>

#include "decode.h"

class nic_t {
 public:
	nic_t();
	reg_t load_uint64();
	void store_uint64(reg_t data);
 private:
 	int start_client_socket(std::string ip_address, uint16_t port);
 	int _nic_fd = -1;
};

#endif