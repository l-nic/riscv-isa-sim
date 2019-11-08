#ifndef _RISCV_NIC_H
#define _RISCV_NIC_H

#include <string>
#include <vector>
#include <thread>
#include <map>
#include <queue>
#include <mutex>

#include "decode.h"

const int CONN_BACKLOG = 32;
const uint64_t INVALID_ID = 0;
const uint64_t MAP_ID = 1; // This is very wasteful reading these id's in all the time.
// We probably want to be able to configure one message type and then have it stick around
// for a while.
const uint64_t REDUCE_ID = 2;
// Host id's, not message id's
const uint64_t ROOT_ID = 1;
const uint64_t BRANCHING_FACTOR = 4;
const std::string ID_ADDR_FILENAME = "/home/vagrant/firechip/lnic-dev/riscv-isa-sim/riscv/id_addr.txt";
const uint64_t LISTEN_CONNECT_DELAY_MS = 3000;

class nic_t {
 public:
	nic_t();
	~nic_t();
	reg_t load_uint64();
	void store_uint64(reg_t data);
	bool lnic_ready();
	bool get_enable();
	void set_enable(reg_t data);
 private:
	struct connection_t {
		int connectionfd;
		uint16_t port;
		std::string ip_address;
	};
	// Might want to see how feasible it is to use protobufs here.
	struct Message {
		uint64_t message_type_id = INVALID_ID;
		uint64_t destination_id = INVALID_ID;
	};
	struct MapMessage : Message {
		uint64_t max_depth = 3;
		uint64_t cur_depth = 0;
		uint64_t src_host_id = INVALID_ID;
	};
	struct ReduceMessage : Message {

	};
	struct MessageState {
		uint64_t response_cnt = 0;
		uint64_t map_cnt = BRANCHING_FACTOR;
		uint64_t src_host_id = INVALID_ID;
	};

	int start_client_socket(std::string ip_address, uint16_t port);
 	int start_server_socket(uint16_t port);
 	void handle_connection(connection_t conn);
 	std::vector<std::string> split(std::string input, std::string delim);
 	std::map<uint64_t, connection_t> get_id_addr_map();
 	void flush_write_queue();
 	uint64_t get_child_id(uint64_t own_id, uint64_t i);
 	void listen_for_connections();

 	std::vector<std::thread> all_threads;
	std::mutex read_lock;
	std::queue<Message*> read_queue;
	std::queue<Message*> write_queue;
	std::map<uint64_t, uint64_t> message_ids;
	Message* current_message = nullptr;
	uint64_t message_index = 0;
	Message* current_write_message = nullptr;
	uint64_t write_message_index = 0;
	std::vector<uint64_t> server_ids;
	std::map<uint64_t, connection_t> id_addr_map;
	int server_socket = -1;
	uint64_t own_id = INVALID_ID;
	bool enable = false;
};

#endif
