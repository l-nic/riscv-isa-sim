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
#define SERVER_PORT 5000

using namespace std;

nic_t::nic_t(struct nic_config_t* nic_config) {
	if (nic_config == nullptr) {
		printf("Nic configuration is null\n");
		exit(-1);
	}
	if (nic_config->treelet_id == -1) {
		printf("Treelet id is not set\n");
		exit(-1);
	}
	_treelet_id = nic_config->treelet_id;
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
    printf("Spike nic initialized\n");
}

nic_t::~nic_t() {
    _receive_thread.join();
}

reg_t nic_t::read_uint64() {
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
	    printf("initialized current message with size %d\n", _current_message.size);
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
	if (_out_message == nullptr) {
		_out_message = new struct nic_t::message_t;
		_out_message->size = sizeof(uint64_t);
		_out_message->data = new char[sizeof(uint32_t)*2]; // TODO: The actual header will probably be easier to deal with than this one
		memcpy(_out_message->data, &data, sizeof(uint64_t));
		_out_message_index = 0;
		//printf("started message\n");
	} else if (_out_message->size == sizeof(uint64_t)) {
		uint32_t* data_parsed = (uint32_t*)&data;
		_out_message->size = data_parsed[0] + 3*sizeof(uint32_t);
		char temp_header[sizeof(uint32_t)*2];
		memcpy(temp_header, _out_message->data, sizeof(uint32_t)*2);
		delete [] _out_message->data;
		uint32_t buf_size = _out_message->size;
		buf_size += sizeof(uint64_t) - (buf_size % sizeof(uint64_t)); // Round the buffer size up to a multiple of the word size.
		_out_message->data = new char[buf_size];
		memcpy(_out_message->data, temp_header, sizeof(uint32_t)*2);
		memcpy(_out_message->data + sizeof(uint64_t), &data, sizeof(uint64_t));
		_out_message_index = 2*sizeof(uint64_t);
		//printf("init message index is %d\n", _out_message_index);
	} else {
		memcpy(_out_message->data + _out_message_index, &data, sizeof(uint64_t));
		_out_message_index += sizeof(uint64_t);
		//printf("in message index is %d\n", _out_message_index);
	}

	if (_out_message_index >= _out_message->size) {
		// Actually write out the message
		for (int i = 0; i < _out_message->size; i++) {
			uint8_t data = (uint8_t)_out_message->data[i];
			printf("%d, %d\n", i, data);
		}
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
		uint32_t header_buf[2];
		ssize_t total_len = 0;
		ssize_t actual_len = 0;
		do {
			actual_len = read(_switch_fd, (char*)&header_buf + total_len, 2*sizeof(uint32_t) - total_len);
			total_len += actual_len;
			if (actual_len <= 0) {
				printf("Receive error, exiting\n");
				exit(-1);
			}
		} while (total_len < 2*sizeof(uint32_t));
		printf("message id is %d and size is %d\n", header_buf[0], header_buf[1]);

		uint32_t buf_size = header_buf[1] + 2*sizeof(uint32_t);
		buf_size += sizeof(uint64_t) - (buf_size % sizeof(uint64_t)); // Round up to 64-bit words
		char* buffer = new char[buf_size];
		memcpy(buffer, header_buf, 2*sizeof(uint32_t));
		total_len = 0;
		do {
			actual_len = read(_switch_fd, buffer + 2*sizeof(uint32_t) + total_len, header_buf[1] - total_len);
			total_len += actual_len;
			if (actual_len <= 0) {
				printf("Receive error, exiting\n");
				exit(-1);
			}
		} while (total_len < header_buf[1]);
		if (header_buf[1] + 2*sizeof(uint32_t) < buf_size) {
			// Pad with zeros if needed
			memset(buffer + 2*sizeof(uint32_t) + header_buf[1], 0, buf_size - header_buf[1] - 2*sizeof(uint32_t));
		}
		printf("received full message\n");

		_message_queue_lock.lock();
		_messages.push({buffer, buf_size});
		_message_queue_lock.unlock();
		printf("pushed message into queue\n");
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
    ssize_t written_len = write(sockfd, &_treelet_id, sizeof(uint32_t));
    if (written_len <= 0) {
        return -1;
    }
    _switch_fd = sockfd;
    return 0;
}