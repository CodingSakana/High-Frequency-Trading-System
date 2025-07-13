#include "tcp_socket.h"

namespace Common
{
/* iface 就是网络接口名，比如 eth0 */
/// Create TCPSocket with provided attributes to either listen-on / connect-to.
auto TCPSocket::connect(const std::string& ip, const std::string& iface, int port, bool is_listening) -> int {
    // Note that needs_so_timestamp=true for FIFOSequencer.
    const SocketCfg socket_cfg{ip, iface, port, false, is_listening, true};
    socket_fd_ = createSocket(logger_, socket_cfg);

    socket_attrib_.sin_addr.s_addr = INADDR_ANY;
    socket_attrib_.sin_port = htons(port);
    socket_attrib_.sin_family = AF_INET;

    return socket_fd_;
}

/**
 * 在非阻塞 TCP socket 上：
 *      接收网络数据（包括内核提供的接收时间戳）
 *      发送已准备好的发送数据
 *      触发回调处理已收到的数据
 */
/// Called to publish outgoing data from the buffers as well as check for and callback if data is available in the
/// read buffers.
auto TCPSocket::sendAndRecv() noexcept -> bool {
    /* CMSG_SPACE 宏是计算存储一个 struct timeval 控制消息所需的总空间（包括头部 + 对齐 padding） */
    /* ctrl[]：为内核控制信息准备的缓冲区（这里用来接收“时间戳”） */
    char ctrl[CMSG_SPACE(sizeof(struct timeval))];
    /* cmsg：控制信息的头指针 */
    auto cmsg = reinterpret_cast<struct cmsghdr*>(&ctrl);

    /* iov：描述了你打算接收的数据应该存放到哪里（buffer + 剩余空间） */
    iovec iov{inbound_data_.data() + next_rcv_valid_index_, TCPBufferSize - next_rcv_valid_index_};

    /* msg：最终传给 recvmsg()，描述“我要收什么数据+收哪些元信息” */
    /*
        struct msghdr {
            void         *msg_name;       // 来源地址结构（IN/OUT）
            socklen_t     msg_namelen;    // 来源地址长度（IN/OUT）
            struct iovec *msg_iov;        // 数据缓冲区（IN）
            int           msg_iovlen;     // 缓冲区数量（IN）
            void         *msg_control;    // 控制消息缓冲区（IN/OUT）
            socklen_t     msg_controllen; // 控制消息长度（IN/OUT）
            int           msg_flags;      // 接收标志（OUT）
        };
     */
    msghdr msg{&socket_attrib_, sizeof(socket_attrib_), &iov, 1, ctrl, sizeof(ctrl), 0};

    // Non-blocking call to read available data.
    const auto read_size = recvmsg(socket_fd_, &msg, MSG_DONTWAIT);
    if (read_size > 0) {
        next_rcv_valid_index_ += read_size;

        Nanos kernel_time = 0;
        timeval time_kernel;
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMP &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(time_kernel))) {
            memcpy(&time_kernel, CMSG_DATA(cmsg), sizeof(time_kernel));
            kernel_time = time_kernel.tv_sec * NANOS_TO_SECS +
                          time_kernel.tv_usec * NANOS_TO_MICROS; // convert timestamp to nanoseconds.
        }

        const auto user_time = getCurrentNanos();

        logger_.log("%:% %() % read socket:% len:% utime:% ktime:% diff:%\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), socket_fd_, next_rcv_valid_index_, user_time, kernel_time,
                    (user_time - kernel_time));
        recv_callback_(this, kernel_time);
    }

    if (next_send_valid_index_ > 0) {
        // Non-blocking call to send data.
        const auto n = ::send(socket_fd_, outbound_data_.data(), next_send_valid_index_, MSG_DONTWAIT | MSG_NOSIGNAL);
        logger_.log("%:% %() % send socket:% len:%\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), socket_fd_, n);
    }
    next_send_valid_index_ = 0;

    return (read_size > 0);
}

/* 只是发送到 outbound_data_ 缓存中 */
/// Write outgoing data to the send buffers.
auto TCPSocket::send(const void* data, size_t len) noexcept -> void {
    memcpy(outbound_data_.data() + next_send_valid_index_, data, len);
    next_send_valid_index_ += len;
}

/** 这是新加的默认回调函数 */
auto TCPSocket::defaultRecvCallback(TCPSocket* socket, Nanos rx_time) noexcept {
    logger_.log("%:% %() % TCPSocket::defaultRecvCallback() socket:% len:% rx:%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), socket->socket_fd_, socket->next_rcv_valid_index_, rx_time);
}
} // namespace Common
