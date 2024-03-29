#include "ds.grpc.pb.h"
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <memory>
#include <deque>
#include <future>
#include <thread>

#include "constants.h"
#include "helper.h"

using ds::gRPCService;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerWriter;
using grpc::Status;
using grpc::Channel;
using grpc::ClientContext;

class gRPCServiceImpl final : public gRPCService::Service {
private:
    enum class BlockState{ DISK, LOCKED, MEMORY};
    typedef struct {
        BlockState state;
        int length;
        std::string data;
    } Info;
    std::string filename;
    enum class BackupState {ALIVE, DEAD, REINTEGRATION};
    std::atomic<BackupState> backup_state;
    enum class ServerState: int32_t { PRIMARY = 0, BACKUP };
    std::atomic<ServerState> current_server_state_;
    std::unique_ptr <gRPCService::Stub> stub_;
    std::unordered_map<int, Info*> temp_data;
    using fut_t = std::future<std::optional<int>>;
    std::deque<fut_t> pending_futures;
    std::mutex reintegration_lock;
    std::vector<std::mutex> per_block_locks;
    int fd;

public:
    void create_file(const std::string filename) {
        fd = ::open(filename.c_str(), O_RDWR|O_CREAT, S_IRWXU);
        if (fd == -1) {
            LOG_ERR_MSG("unable to open fd for: ", filename);
        }
    }

    int write(const char* buf, int address, int size) {
        LOG_DEBUG_MSG("writing @", address, ": ", std::string(buf, size));
        const int written_b = ::pwrite(fd, buf, size, address);
        if (written_b == -1) {
            LOG_ERR_MSG("write failed @ ", address, " ", errno);
        }
        return written_b;
    }

    int read(char* buf, int address, int size) {
        const int read_b = ::pread(fd, buf, size, address);
        if (read_b == -1) {
            LOG_ERR_MSG("read failed @ ", address, " ", errno);
        }
        LOG_DEBUG_MSG("reading @", address, ":", size, " -> ", std::string(buf, read_b));
        return read_b;
    }

    auto get_server_state() const {
        return current_server_state_.load(std::memory_order_relaxed);
    }
    void transition(ServerState current_state) {
        ASS(get_server_state() == current_state, "server state transition error");
        ASS(current_server_state_.compare_exchange_strong(current_state, ServerState::PRIMARY),
            "concurrent attempt to do state transition?");
    }
    void transition_to_primary() {
        transition(ServerState::BACKUP);
        LOG_INFO_MSG(" -> PRIMARY");
    }
    void transition_to_backup() {
        transition(ServerState::PRIMARY);
        LOG_INFO_MSG(" -> BACKUP");
    }

    explicit gRPCServiceImpl(std::shared_ptr<Channel> channel,
                             const std::string filename, bool primary = true) :
            stub_(gRPCService::NewStub(channel)) {
        std::vector<std::mutex> new_locks(constants::BLOCK_SIZE);
        per_block_locks.swap(new_locks);
        current_server_state_ = (primary)? ServerState::PRIMARY : ServerState::BACKUP;
        this->filename = filename;
        create_file(filename);
        LOG_DEBUG_MSG("Filename ", this->filename, " f:", filename);
        backup_state = BackupState::ALIVE;
        current_server_state_ = primary ? ServerState::PRIMARY : ServerState::BACKUP;
        if (!primary) {
            LOG_DEBUG_MSG("reintegration started at backup")
            secondary_reintegration();
        }
    }

    void wait_before_read(const ds::ReadRequest* readRequest) {
        ASS(current_server_state_ == ServerState::PRIMARY, "waiting for reads in primary shouldn't happen");
        std::vector<int> blocks = get_blocks_involved(readRequest->address(), constants::BLOCK_SIZE);
        // change this to get signaled when the entry is removed from the map (write to that block is complete)
        bool can_read_all = false;
        while(!can_read_all) {
            can_read_all = true;
            for (const int &b: blocks) {
                if (temp_data.count(b) && temp_data[b]->state == BlockState::LOCKED) {
                    can_read_all = false;
                    break;
                }
            }
            if (!can_read_all) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(1));
            }
        }
    }

    Status c_read(ServerContext *context, const ds::ReadRequest *readRequest,
                  ds::ReadResponse *readResponse) {
//        if (current_server_state_ == ServerState::BACKUP) {
//            wait_before_read(readRequest);
//        }
        LOG_DEBUG_MSG("reading from backup");
        int buf_size = readRequest->data_length();
        auto buf = std::make_unique<char[]>(buf_size);
        int bytes_read = read(buf.get(), readRequest->address(), buf_size);
        LOG_DEBUG_MSG(buf.get(), " bytes read");
        readResponse->set_data(buf.get());
        return Status::OK;
    }

    Status hb_check(ServerContext *context, const ds::HBRequest *request, ds::HBResponse *response) {
        if (request->is_primary()) {
//            LOG_DEBUG_MSG("I'm primary");
        } else {
//            LOG_DEBUG_MSG("I'm secondary");
        }

        return Status::OK;
    }
    // Returns the block indices for the address and data_length. In this case return vector size is at most 2
    std::vector<int> get_blocks_involved(const int address, const int data_length) {
        int first_block = address / constants::BLOCK_SIZE;
        int end_of_first_block = (first_block + 1) * constants::BLOCK_SIZE - 1;
        int first_block_size_left = end_of_first_block - address + 1;
        std::vector<int> blocks_involved;
        blocks_involved.push_back(first_block);
        if (data_length > first_block_size_left) {
            blocks_involved.push_back(first_block + 1);
        }
        return blocks_involved;
    }

    void get_write_locks(const ds::WriteRequest *writeRequest) {
        reintegration_lock.lock();
        std::vector<int> blocks = get_blocks_involved(writeRequest->address(), writeRequest->data_length());
        if (blocks.size() == 1) {
            per_block_locks[blocks[0]].lock();
        } else {
            // max size can only be 2, same thing can be generalized to n, no need in this case.
            std::mutex& first_block_lock = per_block_locks[blocks[0]];
            std::mutex& second_block_lock = per_block_locks[blocks[1]];

            bool first_block_lock_acquired = false,
                    second_block_lock_acquired = false;

            while(!first_block_lock_acquired || !second_block_lock_acquired) {
                // try acquiring the locks
                first_block_lock_acquired = !first_block_lock_acquired && first_block_lock.try_lock();
                second_block_lock_acquired = !first_block_lock_acquired && second_block_lock.try_lock();
                // if both obtained, will get out of loop, if not both obtained, release obtained locks
                if (!first_block_lock_acquired || !second_block_lock_acquired) {
                    if (first_block_lock_acquired) {
                        first_block_lock.unlock();
                    }
                    if (second_block_lock_acquired) {
                        second_block_lock.unlock();
                    }
                }
            }
        }
    }

    void release_write_locks(const ds::WriteRequest *writeRequest) {
        std::vector<int> blocks = get_blocks_involved(writeRequest->address(), writeRequest->data_length());
        for (const int &block: blocks) {
            per_block_locks[block].unlock();
        }
        reintegration_lock.unlock();
    }

    Status p_reintegration(ServerContext *context, const ds::ReintegrationRequest* reintegrationRequest,
                           ds::ReintegrationResponse* reintegrationResponse) {

        assert_msg(current_server_state_ != ServerState::PRIMARY, "Reintegration called on backup");
        this->backup_state = BackupState::REINTEGRATION;
        // return the entries in temp_data that are in disk state.
        // opt_todo: stream the entries instead of returning at once
        for (auto it: temp_data) {
            Info *info = it.second;
            if (info->state == BlockState::DISK) {
                int address = it.first;
                reintegrationResponse->add_addresses(address);

                int data_length = info->length;
                reintegrationResponse->add_data_lengths(data_length);
                reintegrationResponse->add_data(info->data);
            }
        }
        return Status::OK;
    }

    Status p_reintegration_phase_two(ServerContext *context, const ds::ReintegrationRequest* reintegrationRequest,
                                     ds::ReintegrationResponse* reintegrationResponse) {

        assert_msg(current_server_state_ != ServerState::PRIMARY, "Reintegration called on backup");

        // pause all writes for now!
        reintegration_lock.lock();

        // return the entries in temp_data that are in MEMORY state.
        // opt_todo: stream the entries instead of returning at once
        for (auto it: temp_data) {
            Info *info = it.second;
            if (info->state == BlockState::MEMORY) {
                reintegrationResponse->add_addresses(it.first);
                reintegrationResponse->add_data_lengths(info->length);
                reintegrationResponse->add_data(info->data);
            }
        }
        return Status::OK;
    }

    Status p_reintegration_complete(ServerContext *context, const ds::ReintegrationRequest* reintegrationRequest,
                                    ds::ReintegrationResponse* reintegrationResponse) {
        this->backup_state = BackupState::ALIVE;
        reintegration_lock.unlock();
        return Status::OK;
    }

    void secondary_reintegration() {
        ClientContext context;
        ds::ReintegrationRequest reintegration_request;
        ds::ReintegrationResponse reintegration_response;
        Status status = stub_->p_reintegration(&context, reintegration_request, &reintegration_response);

        // write all missing writes in the backup
        for (int i = 0; i < reintegration_response.data_size(); i++) {
            write(reintegration_response.data(i).c_str(),
                  reintegration_response.addresses(i),
                  reintegration_response.data_lengths(i));
        }

        // get memory based writes
        reintegration_response.clear_data();
        reintegration_response.clear_data_lengths();
        reintegration_response.clear_addresses();
        status = stub_->p_reintegration_phase_two(&context, reintegration_request, &reintegration_response);

        for (int i = 0; i < reintegration_response.data_size(); i++) {
            write(reintegration_response.data(i).c_str(),
                  reintegration_response.addresses(i),
                  reintegration_response.data_lengths(i));
        }

        // notify primary that reintegration is complete
        reintegration_response.clear_data();
        reintegration_response.clear_data_lengths();
        reintegration_response.clear_addresses();
        status = stub_->p_reintegration_complete(&context, reintegration_request, &reintegration_response);

    }

    Status c_write(ServerContext *context, const ds::WriteRequest *writeRequest,
                   ds::WriteResponse *writeResponse) {
        if (current_server_state_ == ServerState::PRIMARY) {
            LOG_DEBUG_MSG("Starting primary server write");
            while (pending_futures.size()) {
                auto& pf = pending_futures.front();
                if (pf.valid()) {
                    const auto addr = pf.get();
                    if (addr.has_value())
                        temp_data.erase(addr.value());
                    pending_futures.pop_front();
                }
            }

//            get_write_locks(writeRequest);

//            BlockState state = (backup_state == BackupState::REINTEGRATION) ? BlockState::MEMORY : BlockState::DISK;
            Info info = {BlockState::DISK, writeRequest->data_length(), writeRequest->data()};
            // TODO: make map thread safe
            temp_data[(int)writeRequest->address()] = &info;
            if (backup_state == BackupState::ALIVE) {
                ClientContext context;
                ds::AckResponse ackResponse;
                LOG_DEBUG_MSG("sending read to backup");
                Status status = stub_->s_write(&context, *writeRequest, &ackResponse);
                LOG_DEBUG_MSG("back from backup");
            }
            LOG_DEBUG_MSG("write from map to file");

            int flag = write(writeRequest->data().c_str(), writeRequest->address(), writeRequest->data_length());
            if (flag == -1)
                return Status::CANCELLED;
            writeResponse->set_bytes_written(writeRequest->data_length());

            if (backup_state == BackupState::ALIVE) {
                LOG_DEBUG_MSG("commit to backup");
                ClientContext context;
                ds::CommitRequest commitRequest;
                commitRequest.set_address(writeRequest->address());

                const int waddr = writeRequest->address();

                fut_t f = std::async(std::launch::async,
                     [&]() -> std::optional<int> {
                         ClientContext context;
                         ds::CommitRequest commitRequest;
                         commitRequest.set_address(waddr);
                         ds::AckResponse ackResponse;
                         if ((stub_->s_commit(&context, commitRequest, &ackResponse)).ok())
                             return waddr;
                         return std::nullopt;
                     });
                pending_futures.push_back(std::move(f));
                LOG_DEBUG_MSG("committed to backup");
            }
//            release_write_locks(writeRequest);
            return Status::OK;
        }
        LOG_DEBUG_MSG("Starting backup server write");
    }
    Status s_write(ServerContext *context, const ds::WriteRequest *writeRequest,
                   ds::AckResponse *ackResponse) {
        if (current_server_state_ == ServerState::BACKUP) {
            LOG_DEBUG_MSG("Starting backup server write");
            BlockState state = BlockState::LOCKED;
            Info info = {state, writeRequest->data_length(), writeRequest->data()};
            temp_data[(int) writeRequest->address()] = &info;
        } else {
            LOG_DEBUG_MSG("calling s_write at backup?");
        }
        return Status::OK;
    }

    Status s_commit(ServerContext *context, const ds::CommitRequest *commitRequest,
                    ds::AckResponse *ackResponse) {
        LOG_DEBUG_MSG("calling commit on backup");
        BlockState diskState = BlockState::DISK;
        Info *info = temp_data[(int)commitRequest->address()];
        write(info->data.c_str(), commitRequest->address(), info->length);
        temp_data.erase((int)commitRequest->address());
        return Status::OK;
    }

    ~gRPCServiceImpl() {
        LOG_DEBUG_MSG("Calling destructor");
        ::close(fd);
    }
};

int main(int argc, char *argv[]) {
//    if(argc < 7) {
//        printf("Usage : ./server -ip <ip> -port <port> -datafile <datafile>\n");
//        return 0;
//    }

    std::string ip{"0.0.0.0"}, port{std::to_string(constants::BACKUP_PORT)}, datafile{"data"};
    for(int i = 1; i < argc - 1; ++i) {
        if(!strcmp(argv[i], "-ip")) {
            ip = std::string{argv[i+1]};
        } else if(!strcmp(argv[i], "-port")) {
            port = std::string{argv[i+1]};
        } else if (!strcmp(argv[i], "-datafile")) {
            datafile = std::string{argv[i+1]};
        }
    }

    LOG_DEBUG_MSG("Starting backup");
    std::string server_address(ip + ":" + port);
    gRPCServiceImpl service(grpc::CreateChannel("localhost:" + constants::PRIMARY_PORT,
        grpc::InsecureChannelCredentials()), datafile, false);
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.SetMaxSendMessageSize(INT_MAX);
    builder.SetMaxReceiveMessageSize(INT_MAX);
    builder.SetMaxMessageSize(INT_MAX);
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
}
