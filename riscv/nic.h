#ifndef _RISCV_NIC_H
#define _RISCV_NIC_H

#include <string>
#include <vector>
#include <thread>
#include <map>
#include <queue>
#include <mutex>

#include "decode.h"

// Internal socket management and ip translation constants.
const int CONN_BACKLOG = 32;
const std::string ID_ADDR_FILENAME = "/home/vagrant/firechip/lnic-dev/riscv-isa-sim/riscv/id_addr.txt";
const uint64_t DATAGRAM_SIZE = 1024;

// Lnic header magic number.
const uint64_t LNIC_MAGIC = 0xABCDEF12;


class nic_t {
 public:
	nic_t();
	~nic_t();

	// Global configuration interface
	void set_enable(reg_t data);
	bool get_enable();
	void set_own_port_id(uint64_t attempted_port_id);
	uint64_t get_port_id();

	// Read interface
	reg_t load_uint64();
	uint64_t num_messages_ready();
	uint64_t is_last_word_read();
	uint64_t read_src_ip_lower();
	uint64_t read_src_ip_upper();
	uint64_t read_src_port();

	// Write interface
	void store_uint64(reg_t data);
	void set_dst_ip_lower(uint64_t lower_ip_bits);
	void set_dst_ip_upper(uint64_t upper_ip_bits);
	void set_dst_port(uint64_t dst_port_id);
	void write_message_end();
 private:
 	// Reading and ip translation utility functions.
 	void handle_lnic_datagram(uint8_t* datagram, size_t datagram_len, std::string src_ip_address);
 	void listen_for_datagrams();
 	int start_server_socket(uint16_t port);
 	std::vector<std::string> split(std::string input, std::string delim);
 	void populate_addr_maps();
 	void fake_to_real_addr(std::string& real_addr, uint16_t& real_port, std::string& fake_addr);
 	void real_to_fake_addr(std::string& real_addr, uint16_t& real_port, std::string& fake_addr);

 	// Per-message metadata, used for both writing and reading.
 	struct message_data_t {
 		std::string src_ip_addr;
 		std::string dst_ip_addr;
 		uint64_t src_port_id;
 		uint64_t dst_port_id;
 	};

 	// Message header data actually serialized onto the wire.
 	struct header_data_t {
 		uint64_t magic;
 		uint64_t src_port_id;
 		uint64_t dst_port_id;
 	} __attribute__((packed));

 	// Used to track associated metadata for words in the read queue.
 	struct read_word_t {
 		uint64_t word;
 		bool is_last_word;
 		message_data_t* per_message_data;
 	};

 	// Used to track metadata for the message currently being written.
 	struct write_message_t {
 		std::vector<uint64_t> words;
 		message_data_t per_message_data;
 	};

 	// Read queue and support structures, for each thread
	std::map<uint64_t, std::mutex> _read_lock;
	std::map<uint64_t, std::queue<read_word_t>> _read_queue;
	std::map<uint64_t, bool> _is_last_word;
	std::map<uint64_t, message_data_t*> _per_message_data;
	std::map<uint64_t, uint64_t> _num_read_messages;

	// Write message, for each thread
	std::map<uint64_t, write_message_t> _write_message;

	// Internal socket, ip translation accounting, and global enable accounting
	std::string _own_ip_address;
	int _server_socket = -1;
	int _write_socket = -1;
	std::map<std::string, std::pair<std::string, uint16_t>> _real_addr_map;
	std::map<std::pair<std::string, uint16_t>, std::string> _fake_addr_map;
 	std::vector<std::thread> _all_threads;
	bool _enable = false;
};

#endif
