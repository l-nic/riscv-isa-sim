#include "nic.h"

nic_t::nic_t() {

}

void nic_t::store_uint64(reg_t data) {
	hidden_data = data;
}

reg_t nic_t::load_uint64() {
	return hidden_data;
}