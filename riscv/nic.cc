#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <fstream>
#include <string>

#include "nic.h"

#define SERVER_IP_ADDR "127.0.0.1"
#define SERVER_PORT 5001

using namespace std;

nic_t::nic_t(struct nic_config_t* nic_config) {
	if (nic_config == nullptr) {
		printf("Nic configuration is null\n");
		return;
		//exit(-1);
	}
	// if (nic_config->treelet_id == -1) {
	// 	printf("Treelet id is not set\n");
	// 	return;
	// 	//exit(-1);
	// }
	if (nic_config->nic_ip_addr == -1) {
		printf("Nic ip addr is not set\n");
		return;
	}
	_nic_ip_addr = nic_config->nic_ip_addr;
	// if (nic_config_data == nullptr) {
	// 	printf("No nic configuration file specified\n");
	// 	exit(-1);
	// }
	// ifstream config_file(nic_config_data);
	// if (!config_file.is_open()) {
	// 	printf("Unable to open nic configuration file %s\n", nic_config_data);
	// 	exit(-1);
	// }
	// string line;
	// while (getline(config_file, line)) {
	// 	printf("%s\n", line.c_str());
	// 	if (line.find("treelet_id=") == 0) {
	// 		_treelet_id = atoi(line.substr(strlen("treelet_id=")).c_str());
	// 		printf("%d\n", _treelet_id);
	// 	}
	// }
    int client_socket_retval = start_client_socket(SERVER_IP_ADDR, SERVER_PORT);
    if (client_socket_retval < 0) {
    	printf("Unable to connect to switch\n");
    	exit(-1);
    }
    _receive_thread = move(thread([this]() {this->receive_data();}));
	_ran_init = true;
    printf("Spike nic initialized\n");
}

nic_t::~nic_t() {
	if (!_ran_init) return;
    _receive_thread.join();
}

reg_t nic_t::read_uint64() {
    if (!_ran_init) return 0;
	// Mimics the actual nanoPU and throws an error if attempting to read with no messages present in the queue
	if (!_received_first_message || _current_message_index >= _current_message.size) {
	    _message_queue_lock.lock();
	    if (_messages.empty()) {
	    	printf("Attempted to read a message from an empty queue\n");
	    	_message_queue_lock.unlock();
	    	exit(-1);
	    }
	    // If there is an existing message that was just finished, make sure to clean it up first
	    if (_received_first_message) {
	    	delete [] _current_message.data;
	    }
	    _current_message = _messages.front();
	    _messages.pop();
	    _message_queue_lock.unlock();
	    _received_first_message = true;
	    _current_message_index = 0;
	    // printf("initialized current message with size %d\n", _current_message.size);
	}
	// Current message should now be set up
	// for (int i = 0; i < _current_message.size / sizeof(uint32_t); i++) {
	// 	char* offset = _current_message.data + i*sizeof(uint32_t);
	// 	printf("%#x\n", *(uint32_t*)offset);
	// }
	char* message_offset = _current_message.data + _current_message_index;
	uint64_t current_word = *(uint64_t*)message_offset;
	_current_message_index += sizeof(uint64_t);
    return current_word;
}

void nic_t::write_uint64(reg_t data) {
	if (!_ran_init) return;
	if (_out_message == nullptr) {
		_out_message = new nic_t::message_t;
		//printf("Received data %#lx\n", data);
		uint64_t msg_len = data & 0xffff;
		//printf("Creating message with size %d\n", sizeof(uint64_t) + msg_len);
		_out_message->size = sizeof(uint64_t) + msg_len;
		_out_message->data = new char[_out_message->size];
		memcpy(_out_message->data, &data, sizeof(uint64_t));
		_out_message_index = sizeof(uint64_t);
		//printf("started message\n");
	} else {
		memcpy(_out_message->data + _out_message_index, &data, sizeof(uint64_t));
		_out_message_index += sizeof(uint64_t);
	}

	if (_out_message_index >= _out_message->size) {
		// Actually write out the message
		ssize_t total_len = 0;
		ssize_t actual_len = 0;
		do {
			actual_len = write(_switch_fd, _out_message->data + total_len, _out_message->size - total_len);
			total_len += actual_len;
			if (actual_len <= 0) {
				printf("Send error, exiting\n");
				exit(-1);
			}
		} while (total_len < _out_message->size);
		_out_message_index = 0;
		delete [] _out_message->data;
		delete _out_message;
		_out_message = nullptr;
	}

}

uint64_t nic_t::num_messages_ready() {
	if (!_ran_init) return 0;
	// Mimics the actual nanoPU and only returns 0 or 1
	_message_queue_lock.lock();
	uint64_t queue_size_at_check = _messages.size();
	_message_queue_lock.unlock();
	if (queue_size_at_check > 0) {
		return 1;
	}
	return 0;
}

void nic_t::receive_data() {
	while (true) {
		uint64_t header;
		ssize_t total_len = 0;
		ssize_t actual_len = 0;
		do {
			actual_len = read(_switch_fd, (char*)&header + total_len, sizeof(uint64_t) - total_len);
			total_len += actual_len;
			if (actual_len <= 0) {
				printf("Receive error, exiting\n");
				exit(-1);
			}
		} while (total_len < 2*sizeof(uint32_t));
		uint64_t msg_len = header & 0xffff;
		uint32_t buf_size = msg_len + sizeof(uint64_t);
		char* buffer = new char[buf_size];
		memcpy(buffer, &header, sizeof(uint64_t));
		total_len = 0;
		do {
			actual_len = read(_switch_fd, buffer + sizeof(uint64_t) + total_len, msg_len - total_len);
			total_len += actual_len;
			if (actual_len <= 0) {
				printf("Receive error, exiting\n");
				exit(-1);
			}
		} while (total_len < msg_len);
		//printf("received full message\n");

		_message_queue_lock.lock();
		_messages.push({buffer, buf_size});
		_message_queue_lock.unlock();
		//printf("pushed message into queue\n");
	}
}

int nic_t::start_client_socket(const char* ip_address, uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    struct in_addr network_order_address;
    int ip_conversion_retval = inet_aton(ip_address, &network_order_address);
    if (ip_conversion_retval < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = network_order_address;
    int connect_retval = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    if (connect_retval < 0) {
        return -1;
    }
    ssize_t written_len = write(sockfd, &_nic_ip_addr, sizeof(uint32_t));
    if (written_len <= 0) {
        return -1;
    }
    _switch_fd = sockfd;
    return 0;
}