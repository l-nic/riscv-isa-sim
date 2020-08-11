#ifndef _RISCV_NIC_H
#define _RISCV_NIC_H

#include <thread>
#include <mutex>
#include <queue>

#include "decode.h"

class nic_t {
 public:
    nic_t(struct nic_config_t* nic_config);
    ~nic_t();
    reg_t read_uint64();
    void write_uint64(reg_t data);
    uint64_t num_messages_ready();

 private:
 	struct message_t {
 		char* data;
 		uint32_t size;
 	};
 	int start_client_socket(const char* ip_address, uint16_t port);
 	int _switch_fd = -1;
 	uint32_t _treelet_id = -1;
 	std::thread _receive_thread;
 	void receive_data();
 	std::queue<struct message_t> _messages;
 	std::mutex _message_queue_lock;
 	struct message_t _current_message;
 	uint32_t _current_message_index = 0;
 	bool _received_first_message = false;
 	struct message_t* _out_message = nullptr;
 	uint32_t _out_message_index = 0;
};


#endif // _RISCV_NIC_H