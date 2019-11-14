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
const uint64_t LNIC_MAGIC = 0xABCDEF12;
const uint64_t DATAGRAM_SIZE = 1024;

class nic_t {
 public:
	nic_t();
	~nic_t();
	reg_t load_uint64();
	void store_uint64(reg_t data);
	uint64_t num_messages_ready();
	void write_message_end();
	bool get_enable();
	void set_enable(reg_t data);
 private:
 	int start_server_socket(uint16_t port);
 	std::vector<std::string> split(std::string input, std::string delim);
 	std::map<uint64_t, connection_t> get_id_addr_map();
 	void listen_for_datagrams();
 	void handle_lnic_datagram(uint8_t* datagram, ssize_t datagram_len, std::string src_ip_address);
 	void fake_to_real_addr(std::string& real_addr, uint16_t& real_port, std::string& fake_addr);
 	void real_to_fake_addr(std::string& real_addr, uint16_t& real_port, std::string& fake_addr);
 	uint64_t get_port_id();

 	// This is the most basic assembly config interface
 	struct message_data_t {
 		std::string src_ip_addr;
 		std::string dst_ip_addr;
 		uint64_t src_port_id;
 		uint64_t dst_port_id;
 	};

 	// These are the bytes that actually get written onto the wire.
 	// For now, it's pretty much bad udp that's here to send along
 	// out-of-band config data the lnic needs.
 	struct header_data_t {
 		uint64_t magic;
 		uint64_t src_port_id;
 		uint64_t dst_port_id;
 	} __attribute__((packed));

 	struct read_word_t {
 		uint64_t word;
 		bool is_last_word;
 		message_data_t* per_message_data;
 	}

 	struct write_message_t {
 		std::vector<uint64_t> words;
 		message_data_t per_message_data;
 	}

	std::map<uint64_t, std::mutex> _read_lock;
	std::map<uint64_t, std::queue<read_word_t>> _read_queue;
	std::map<uint64_t, bool> _is_last_word;
	std::map<uint64_t, message_data_t> _per_message_data;

	std::map<uint64_t, write_message_t> _write_message;

	std::string _own_ip_address;
	int _server_socket = -1;
	int _write_socket = -1;

	std::map<std::string, std::pair<std::string, uint16_t>> _real_addr_map;
	std::map<std::pair<std::string, uint16_t>, std::string> _fake_addr_map;
 	std::vector<std::thread> _all_threads;
	bool _enable = false;
};

#endif
