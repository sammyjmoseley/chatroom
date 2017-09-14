#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <stdio.h>
#include <netdb.h>
#include <set>
#include <string>
#include <chrono>
#include <unistd.h>
#define DEBUG false
#define HR 100000


struct Message {
    int sockfd;
    const char* message;
    unsigned long message_len;

    ~Message(){

    }
};

struct MessageId {
    int from_id;
    int to_id;
    int lc_id;

    bool operator ==(const MessageId a) const {
        return from_id==a.from_id && to_id == a.to_id && lc_id == a.lc_id;
    }
};

size_t message_id_hash( const MessageId & name ) {
    return std::hash<int>()(name.from_id) ^ std::hash<int>()(name.to_id) ^ std::hash<int>()(name.lc_id);
}

template <class T>
class ConcurrentQueue {
    std::queue<T*>* queue;
    std::mutex* mtx;
    std::condition_variable* cv;

    bool available() {
        return !queue->empty();
    }

public:
    ConcurrentQueue() {
        this->queue = new std::queue<T*>();
        this->mtx = new std::mutex();
        this->cv = new std::condition_variable();
    }

    void push(T* elem) {
        std::unique_lock<std::mutex> lck (*mtx);
        try {
            queue->push(elem);
        } catch (std::logic_error&) {
            std::cout << "[exception caught]" << std::endl;
        }
        cv->notify_one();
    }

    T* pop() {
        std::unique_lock<std::mutex> lck(*mtx);
        try {
            this->cv->wait(lck, [this]{return available();});
            T* ret = queue->front();
            queue->pop();
            return ret;
        } catch (std::logic_error&) {
            std::cout << "[exception caught]" << std::endl;
        }
    }
};

long now() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

template <class K, class V>
class ConcurrentMap {
    std::map<K, V>* map;
    std::mutex* mtx;
public:
    ConcurrentMap () {
        map = new std::map<K, V>();
        mtx = new std::mutex();
    }

    bool contains(K key) {
        return map->find(key) != map->end();
    }

    typename std::map<K, V>::iterator get(K key) {
        typename std::map<K,V>::iterator it = map->find(key);
        return it;
    };

    void put(K key, V val) {
        map->insert({{key, val}});
    }

    void remove(K key) {
        map->erase(key);
    }

    typename std::map<K, V>* get_map() {
        return map;
    }

    typename std::map<K, V>::iterator it() {

        return map->begin();
    }

    typename std::map<K, V>::iterator end() {
        return map->end();
    }

};

template<class T>
class ArrayList {
    T* list;
    int size;
    int max_size;
    std::mutex* mtx;
public:
    ArrayList() {
        list = new T[20];
        size = 0;
        max_size = 20;
        mtx = new std::mutex();
    }

    T get(int idx) {
        std::unique_lock<std::mutex> lck (*mtx);
        T ret;
        try {
            ret = list[idx];
        }
        catch (std::logic_error&) {
            std::cout << "[exception caught]" << std::endl;
        }
        return ret;
    }

    T put(T val) {
        std::unique_lock<std::mutex> lck (*mtx);
        if (size == max_size) {
            int new_size = max_size*2;
            T* new_list = new T[new_size];
            for (int i = 0; i < max_size; i++) {
                new_list[i] = list[i];
            }
            max_size = new_size;
            delete list;
        }
        list[size++] = val;
    }

    int get_size() {
        std::unique_lock<std::mutex> lck (*mtx);
        return size;
    }

};

struct SockProp {
    const int* process;
    long timestamp;
};


struct NodeThread {
    int port_num;
    int my_id;
    std::atomic<bool> is_alive;
    ConcurrentQueue<Message>* message_in_queue;
    ConcurrentQueue<Message>* message_out_queue;
    std::unordered_set<MessageId, decltype(&message_id_hash)>* message_set;
    ConcurrentMap<int, const int*>* process_map; // process_id -> socket_fd
    ConcurrentMap<int, SockProp*>* socket_map; //socket_fd -> process_id
    ArrayList<std::string>* msg_list;

    void (*conn_accepter)(int newsockfd, char* buffer, NodeThread* node_thread);
};

std::ostream* get_logger(NodeThread* nodeThread) {
    std::ostream* fp = &std::cout;
    std::cout << nodeThread->my_id << " ";

    return fp;
}

void error(const char *msg) {
    perror(msg);
    exit(1);
}


void create_socket(NodeThread * node_thread) {

    int sockfd;
    int opt = true;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(node_thread->port_num);

    if( setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&(opt),
                   sizeof(opt)) < 0 ) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    if (bind(sockfd, (struct sockaddr *) &serv_addr,
             sizeof(serv_addr)) < 0) {
        error("ERROR on binding");
    }
    listen(sockfd,5);
    clilen = sizeof(cli_addr);

    fd_set readfds;

    timeval* timeout = new timeval();
    timeout->tv_sec = 1L;
    timeout->tv_usec = 0L;

    while (node_thread -> is_alive) {
        FD_ZERO(&readfds);

        FD_SET(sockfd, &readfds);
        int max_fd = sockfd;
        for (std::map<int, SockProp*>::iterator it = node_thread->socket_map->it();
             it != node_thread->socket_map->end();
             ++it) {
            int sd = it->first;

            if (sd > 0) {
                FD_SET(sd, &readfds);
                if (DEBUG) {
                    *get_logger(node_thread) << "Listening on " << sd << std::endl;
                }
            }
            if (sd > max_fd) {
                max_fd = sd;
            }
        }



        int activity = select(max_fd + 1, &readfds, NULL, NULL, timeout);

        if ((activity < 0) && errno != EINTR) {
            printf("select error");
        }

        if (FD_ISSET(sockfd, &readfds)) {
            int new_socket;
            struct sockaddr_in address;
            int addrlen = sizeof(address);
            if ((new_socket = accept(sockfd,
                                     (struct sockaddr *)&address, (socklen_t*)&addrlen))<0)
            {
                perror("accept");
                exit(EXIT_FAILURE);
            }

            //inform user of socket number - used in send and receive commands
            if (DEBUG) {
                printf("New connection , socket fd is %d , ip is : %s , port : %d \n", new_socket,
                       inet_ntoa(address.sin_addr), ntohs
                       (address.sin_port));
            }

            //send new connection greeting message
//            if( send(new_socket, message, strlen(message), 0) != strlen(message) )
//            {
//                perror("send");
//            }

            SockProp* sockProp = new SockProp();
            sockProp->timestamp = now();
            node_thread->socket_map->put(new_socket, sockProp);


        }

        //else its some IO operation on some other socket
        std::set<int> removeBucket;
        char* buffer = new char[1024];
        for (auto it = node_thread->socket_map->it();
             it != node_thread->socket_map->end();
                ) {
            int sd = it->first;
            bool removed = false;
            if (FD_ISSET( sd , &readfds)) {
                //Check if it was for closing , and also read the
                //incoming message
                int valread;
                sockaddr_in address;
                int addrlen = sizeof(addrlen);



                if ((valread = read( sd , buffer, 1024)) == 0) {
                    //Somebody disconnected , get his details and print
                    getpeername(sd , (struct sockaddr*)&address , \
                        (socklen_t*)&addrlen);
                    if (DEBUG) {
                        printf("Host disconnected , ip %s , port %d \n",
                               inet_ntoa(address.sin_addr), ntohs(address.sin_port));
                    }

                    //Close the socket and mark as 0 in list for reuse
                    close( sd );
                    int* k = (int*) it->second->process;
                    if (k != NULL && node_thread->process_map->contains(*k)) {
                        node_thread->process_map->remove(*it->second->process);
                    }
                    removeBucket.insert(it->first);
                }

                    //Echo back the message that came in
                else {


                    //set the string terminating NULL byte on the end
                    //of the data read
                    buffer[valread] = '\0';

                    char* new_buffer = NULL;
                    int msg_start = 0;
                    unsigned long length = 0UL;
                    if (DEBUG) {
                        std::cout << node_thread->my_id << " recieved : " << buffer << std::endl;
                    }
                    for (; msg_start < valread; msg_start += length+1) {
                        for (length = 0UL; buffer[msg_start + length] != '\n' && buffer[msg_start+length != '\0']; length++) {}
                        new_buffer = new char[length];
                        for (int i = 0; i < length; i++) {
                            new_buffer[i] = buffer[msg_start+i];
                        }

//                        std::cout << "message: " << new_buffer << std::endl;

                        auto message = new Message();
                        message->message = new_buffer;
                        message->message_len = length;
                        message->sockfd = sd;
                        node_thread->message_in_queue->push(message);
                    }



                }
            }

            ++it;
        }

        for (auto it = removeBucket.begin(); it != removeBucket.end(); it++) {
            node_thread->socket_map->remove(*it);
        }
    }
}
struct Command {
    std::string* broadcast;
    hostent* connect;

    Command() {

    }

    ~Command() {

    }
};

const std::string HEARTBEAT_ID("heartbeat ");
const std::string BROADCAST("broadcast ");
const std::string HEARTBEAT("heartbeat");
const std::string DEADSIGNL("deadsignl ");
const std::string CONNECT("connect ");
const std::string MESSAGE("message ");
const std::string ALIVE("alive");
const std::string GET("get");

std::string* get_string(std::string* msg, const std::string* header) {
    if (msg->length() < header->length()) {
        return NULL;
    }
    long pos;
    long end_pos;
    if ((pos = msg->find(*header)) == -1) {
        return NULL;
    }
    if ((end_pos = msg->find('\n')) == -1) {
        end_pos = msg->length();
    }
    std::string* ret = new std::string(msg->substr(pos+header->length(), end_pos-pos-header->length()).c_str());
    return ret;
}

void parse_broadcast(Command* command, std::string* msg) {
    std::string* broadcast = get_string(msg, &BROADCAST);
    command->broadcast = broadcast;
}

void parse_heartbeat(NodeThread *nodeThread, int sockfd, std::string *msg) {
    std::string* str_beat = get_string(msg, &HEARTBEAT);
    if (str_beat == NULL) {
        return;
    }

    nodeThread->socket_map->get(sockfd)->second->timestamp = now();
}

void parse_message(NodeThread *nodeThread, std::string *msg) {
    std::string* str_msg = get_string(msg, &MESSAGE);
    if (str_msg == NULL) {
        return;
    }
    if (DEBUG) {
        *get_logger(nodeThread) << "Got message: " << str_msg->c_str() << std::endl;
    }
    nodeThread->msg_list->put(*str_msg);
}

void get_messages(NodeThread *nodeThread, std::string *msg) {
    std::string* str_msg = get_string(msg, &GET);
    if (str_msg == NULL) {
        return;
    }
    std::string* str = new std::string("messages ");
    for (int i = 0; i < nodeThread->msg_list->get_size(); i++) {
        str->append(nodeThread->msg_list->get(i));
        str->append(",");
    }
    std::string msg_out = str->substr(0,str->length()-1);
    msg_out.append("\n");
    if (DEBUG) {
        *get_logger(nodeThread) << msg_out << std::endl;
    }
    if (nodeThread->process_map->contains(-1)) {
        Message *message = new Message();
        message->sockfd = *nodeThread->process_map->get(-1)->second;
        message->message = msg_out.c_str();
        message->message_len = msg_out.length();
        nodeThread->message_out_queue->push(message);
    }
}

void parse_id_update(NodeThread *nodeThread, int sockfd, std::string *msg) {
    std::string* str_id = get_string(msg, &HEARTBEAT_ID);
    if (str_id == NULL) {
        return;
    }
    int id = atoi(&str_id->front());
    if (DEBUG) {
        *get_logger(nodeThread) << "got id for " << id << std::endl;
    }
    if (!nodeThread->socket_map->contains(sockfd)) {
        nodeThread->socket_map->put(sockfd, NULL);
    }

    if (!nodeThread->process_map->contains(id)) {
        nodeThread->process_map->put(id, NULL);
    }

    nodeThread->socket_map->get(sockfd)->second->process = &nodeThread->process_map->get(id)->first;
    nodeThread->process_map->get(id)->second = &nodeThread->socket_map->get(sockfd)->first;
}

void parse_alive(NodeThread* nodeThread, std::string* msg) {
    std::string* str_alive = get_string(msg, &ALIVE);
    if (str_alive == NULL) {
        return;
    }
    auto id_set = new std::set<int, std::less<int>>();
    id_set->insert(nodeThread->my_id);
    for (auto it = nodeThread->process_map->it(); it != nodeThread->process_map->end(); ++it) {
        id_set->insert(it->first);
    }

    auto str = new std::string("alive ");
    for (auto it = id_set->begin(); it != id_set->end(); ++it) {
        if (*it == -1) {
            continue;
        }
        str->append(std::to_string(*it));
        str->append(",");
    }
    std::string* msg_out = new std::string(str->substr(0,str->length()-1));
    msg_out->append("\n");
    if (DEBUG) {
        *get_logger(nodeThread) << "writing alive: " << msg_out << std::endl;
    }
    if (nodeThread->process_map->contains(-1)) {
        Message *message = new Message();
        message->sockfd = *nodeThread->process_map->get(-1)->second;
        message->message = msg_out->c_str();
        message->message_len = msg_out->length();
        nodeThread->message_out_queue->push(message);
    }
}

void parse_connect(NodeThread* nodeThread, std::string* msg) {
    std::string* str_connect = get_string(msg, &CONNECT);
    if (str_connect == NULL) {
        return;
    }
    long space_pos = str_connect->find(" ");
    long semi_pos = str_connect->find(":");
    long end_line = str_connect->find("\n");
    std::string id = str_connect->substr(0, space_pos);
    if (nodeThread->process_map->contains(atoi(id.c_str()))) {
        return;
    }
    std::string host = str_connect->substr(space_pos + 1UL, semi_pos-space_pos-1UL);
    std::string port = str_connect->substr(semi_pos + 1UL, end_line-semi_pos-1UL);

    struct addrinfo hints, *res;
    int sockfd;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    connect(sockfd, res->ai_addr, res->ai_addrlen);

    SockProp* sockProp = new SockProp();
    sockProp->process = NULL;
    sockProp->timestamp = now();

    nodeThread->socket_map->put(sockfd, sockProp);
}

Command* command_parse_message(std::string* msg) {
    Command* command = new Command();
    parse_broadcast(command, msg);
    return command;
}

void message_reader(NodeThread * nodeThread) {
    while (nodeThread->is_alive.load()) {
        Message* txt = nodeThread->message_in_queue->pop();
        if (DEBUG) {
            *get_logger(nodeThread) << "Recieved on <" << txt->sockfd << "> message <" << txt->message << ">" << std::endl;
        }
        std::string* msg = new std::string(txt->message);
        Command* command = command_parse_message(msg);

        if (command->broadcast != NULL) {
            std::string msg_body("message ");
            msg_body.append(*command->broadcast);
            if (msg_body.find("\n") != msg_body.length()-1) {
                msg_body.append("\n");
            }
            nodeThread->msg_list->put(*command->broadcast);

            for (auto it = nodeThread->process_map->it(); it != nodeThread->process_map->end(); ++it) {
                auto msg_out = new Message();
                msg_out->sockfd = *it->second;
                msg_out->message = msg_body.c_str();
                msg_out->message_len = msg_body.length();
                nodeThread->message_out_queue->push(msg_out);
            }
            if (DEBUG) {
                std::cout << msg_body.c_str() << std::endl;
            }
        }

        parse_heartbeat(nodeThread, txt->sockfd, msg);
        parse_id_update(nodeThread, txt->sockfd, msg);
        parse_alive(nodeThread, msg);
        parse_connect(nodeThread, msg);
        parse_message(nodeThread, msg);
        get_messages(nodeThread, msg);
    }
}

void message_writer(NodeThread* nodeThread) {
    while(nodeThread->is_alive.load()) {
        Message* packet = nodeThread->message_out_queue->pop();
        if (DEBUG) {
            *get_logger(nodeThread) << "Writing on <" << packet->sockfd << "> message <" << packet->message << ">" << std::endl;
        }
        write(packet->sockfd, packet->message, packet->message_len);
    }
}

void heart_pump(NodeThread* nodeThread) {
    std::string* heart_msg = new std::string(HEARTBEAT_ID);
    heart_msg->append(std::to_string(nodeThread->my_id));
    heart_msg->append("\n");
    while(nodeThread->is_alive.load()) {
        usleep(HR);
        for (auto it = nodeThread->socket_map->it(); it != nodeThread->socket_map->end(); ++it) {
            Message* message = new Message();
            message->sockfd = it->first;
            message->message = heart_msg->c_str();
            message->message_len = heart_msg ->length();
            nodeThread->message_out_queue->push(message);
        }
    }
}

void clear_sockets(NodeThread* nodeThread) {
    std::map<int, long>* timestamps = new std::map<int, long>();
    while(nodeThread->is_alive.load()) {
        usleep(3*HR);
        std::set<int>* old_socket_fd = new std::set<int>();
        std::set<int>* old_proceesses = new std::set<int>();

        for (auto it = nodeThread->socket_map->it() ; it != nodeThread->socket_map->end(); ++it) {
            if (timestamps->find(it->first) == timestamps->end()) {
                timestamps->insert({{it->first, it->second->timestamp}});
            } else {
                if (timestamps->find(it->first)->second >= it->second->timestamp) {
                    old_socket_fd->insert(it->first);
                } else {
                    timestamps->erase(it->first);
                    timestamps->insert({{it->first, it->second->timestamp}});
                }
            }
        }

        for (auto it = nodeThread->process_map->it(); it != nodeThread->process_map->end(); ++it) {
            if (old_socket_fd->find(*it->second) != old_socket_fd->end()) {
                old_proceesses->insert(it->first);
            }
        }

        for (int fd : *old_socket_fd) {
            nodeThread->socket_map->remove(fd);
        }

        for (int ps : *old_proceesses) {
            nodeThread->process_map->remove(ps);
        }


    }
}



int main(int argc, char * argv[]) {
    if (argc != 4) {
        error("Incorrect number of arguments, 3 required");
    }
    int id = atoi(argv[1]);
    int n = atoi(argv[2]);
    int port = atoi(argv[3]);


    const int max_num_sockets = 4;

    std::unordered_set<MessageId, decltype(&message_id_hash)> message_set(100, message_id_hash);

    NodeThread* nodeThread = new NodeThread();
//    nodeThread.address = "localhost";
    nodeThread->my_id = id;
    nodeThread->message_in_queue = new ConcurrentQueue<Message>();
    nodeThread->message_out_queue = new ConcurrentQueue<Message>();
    nodeThread->message_set = &message_set;
    nodeThread->is_alive.store(true);
    nodeThread->socket_map = new ConcurrentMap<int, SockProp*>();
    nodeThread->process_map = new ConcurrentMap<int, const int*>();
    nodeThread->port_num = port;
    nodeThread->msg_list = new ArrayList<std::string>();


    std::thread connection_thread(create_socket, nodeThread);

    std::thread reading_thread(message_reader, nodeThread);

    std::thread writing_thread(message_writer, nodeThread);

    std::thread heart(heart_pump, nodeThread);

    std::thread cleanup(clear_sockets, nodeThread);

    connection_thread.join();
    reading_thread.join();
    writing_thread.join();
    heart.join();

    std::cout << std::endl;

    exit(0);
}