#include "mavlink_interface.h"

MavlinkInterface::MavlinkInterface() :
    received_first_actuator_(false),
    qgc_socket_fd_(0),
    sdk_socket_fd_(0), 
    simulator_socket_fd_(0),
    simulator_tcp_client_fd_(0),
    serial_enabled_(false),
    m_status {},
    m_buffer {},
    mavlink_addr_str_("INADDR_ANY"),
    mavlink_udp_port_(kDefaultMavlinkUdpPort),
    mavlink_tcp_port_(kDefaultMavlinkTcpPort),
    qgc_udp_port_(kDefaultQGCUdpPort),
    sdk_udp_port_(kDefaultSDKUdpPort),
    qgc_addr_("INADDR_ANY"),
    sdk_addr_("INADDR_ANY"),
    io_service(),
    serial_dev(io_service),
    device_(kDefaultDevice),
    baudrate_(kDefaultBaudRate),
    tx_q {},
    rx_buf {},
    baro_updated_(false),
    diff_press_updated_(false),
    mag_updated_(false) {

}

MavlinkInterface::~MavlinkInterface() {
  close();
}

void MavlinkInterface::Load()
{
  mavlink_addr_ = htonl(INADDR_ANY);
  if (mavlink_addr_str_ != "INADDR_ANY") {
    mavlink_addr_ = inet_addr(mavlink_addr_str_.c_str());
    if (mavlink_addr_ == INADDR_NONE) {
      std::cerr << "Invalid mavlink_addr: " << mavlink_addr_str_ << ", aborting\n";
      abort();
    }
  }
  local_qgc_addr_.sin_port = 0;
  if (qgc_addr_ != "INADDR_ANY") {
    local_qgc_addr_.sin_port = inet_addr(qgc_addr_.c_str());
    if (local_qgc_addr_.sin_port == 0) {
      std::cerr << "Invalid qgc_addr: " << qgc_addr_ << ", aborting\n";
      abort();
    }
  }
  if (sdk_addr_ != "INADDR_ANY") {
    local_sdk_addr_.sin_port = inet_addr(sdk_addr_.c_str());
    if (local_sdk_addr_.sin_port == 0) {
      std::cerr << "Invalid sdk_addr: " << sdk_addr_ << ", aborting\n";
      abort();
    }
  }

  if (hil_mode_) {

    local_qgc_addr_.sin_family = AF_INET;
    local_qgc_addr_.sin_port = htons(0);
    local_qgc_addr_len_ = sizeof(local_qgc_addr_);

    remote_qgc_addr_.sin_family = AF_INET;
    remote_qgc_addr_.sin_port = htons(qgc_udp_port_);
    remote_qgc_addr_len_ = sizeof(remote_qgc_addr_);

    local_sdk_addr_.sin_family = AF_INET;
    local_sdk_addr_.sin_port = htons(0);
    local_sdk_addr_len_ = sizeof(local_sdk_addr_);

    remote_sdk_addr_.sin_family = AF_INET;
    remote_sdk_addr_.sin_port = htons(sdk_udp_port_);
    remote_sdk_addr_len_ = sizeof(remote_sdk_addr_);

    if ((qgc_socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
      std::cerr << "Creating QGC UDP socket failed: " << strerror(errno) << ", aborting\n";
      abort();
    }

    if (bind(qgc_socket_fd_, (struct sockaddr *)&local_qgc_addr_, local_qgc_addr_len_) < 0) {
      std::cerr << "QGC UDP bind failed: " << strerror(errno) << ", aborting\n";
      abort();
    }

    if ((sdk_socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
      std::cerr << "Creating SDK UDP socket failed: " << strerror(errno) << ", aborting\n";
      abort();
    }

    if (bind(sdk_socket_fd_, (struct sockaddr *)&local_sdk_addr_, local_sdk_addr_len_) < 0) {
      std::cerr << "SDK UDP bind failed: " << strerror(errno) << ", aborting\n";
      abort();
    }

  }

  if (serial_enabled_) {
    // Set up serial interface
    io_service.post(std::bind(&MavlinkInterface::do_read, this));

    // run io_service for async io
    io_thread = std::thread([this] () {
      io_service.run();
    });
    open();

  } else {
    memset((char *)&remote_simulator_addr_, 0, sizeof(remote_simulator_addr_));
    remote_simulator_addr_.sin_family = AF_INET;
    remote_simulator_addr_len_ = sizeof(remote_simulator_addr_);

    memset((char *)&local_simulator_addr_, 0, sizeof(local_simulator_addr_));
    local_simulator_addr_.sin_family = AF_INET;
    local_simulator_addr_len_ = sizeof(local_simulator_addr_);

    if (use_tcp_) {

      local_simulator_addr_.sin_addr.s_addr = htonl(mavlink_addr_);
      local_simulator_addr_.sin_port = htons(mavlink_tcp_port_);

      if ((simulator_socket_fd_ = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Creating TCP socket failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      int yes = 1;
      int result = setsockopt(simulator_socket_fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
      if (result != 0) {
        std::cerr << "setsockopt failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      struct linger nolinger {};
      nolinger.l_onoff = 1;
      nolinger.l_linger = 0;

      result = setsockopt(simulator_socket_fd_, SOL_SOCKET, SO_LINGER, &nolinger, sizeof(nolinger));
      if (result != 0) {
        std::cerr << "setsockopt failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      // The socket reuse is necessary for reconnecting to the same address
      // if the socket does not close but gets stuck in TIME_WAIT. This can happen
      // if the server is suddenly closed, for example, if the robot is deleted in gazebo.
      int socket_reuse = 1;
      result = setsockopt(simulator_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &socket_reuse, sizeof(socket_reuse));
      if (result != 0) {
        std::cerr << "setsockopt failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      // Same as above but for a given port
      result = setsockopt(simulator_socket_fd_, SOL_SOCKET, SO_REUSEPORT, &socket_reuse, sizeof(socket_reuse));
      if (result != 0) {
        std::cerr << "setsockopt failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      // set socket to non-blocking
      result = fcntl(simulator_socket_fd_, F_SETFL, O_NONBLOCK);
      if (result == -1) {
        std::cerr << "setting socket to non-blocking failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      if (bind(simulator_socket_fd_, (struct sockaddr *)&local_simulator_addr_, local_simulator_addr_len_) < 0) {
        std::cerr << "bind failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      errno = 0;
      if (listen(simulator_socket_fd_, 0) < 0) {
        std::cerr << "listen failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      memset(fds_, 0, sizeof(fds_));
      fds_[LISTEN_FD].fd = simulator_socket_fd_;
      fds_[LISTEN_FD].events = POLLIN; // only listens for new connections on tcp

    } else {
      remote_simulator_addr_.sin_addr.s_addr = mavlink_addr_;
      remote_simulator_addr_.sin_port = htons(mavlink_udp_port_);

      local_simulator_addr_.sin_addr.s_addr = htonl(INADDR_ANY);
      local_simulator_addr_.sin_port = htons(0);

      if ((simulator_socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        std::cerr << "Creating UDP socket failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      // set socket to non-blocking
      int result = fcntl(simulator_socket_fd_, F_SETFL, O_NONBLOCK);
      if (result == -1) {
        std::cerr << "setting socket to non-blocking failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      if (bind(simulator_socket_fd_, (struct sockaddr *)&local_simulator_addr_, local_simulator_addr_len_) < 0) {
        std::cerr << "bind failed: " << strerror(errno) << ", aborting\n";
        abort();
      }

      memset(fds_, 0, sizeof(fds_));
      fds_[CONNECTION_FD].fd = simulator_socket_fd_;
      fds_[CONNECTION_FD].events = POLLIN | POLLOUT; // read/write
    }
  }
}

void MavlinkInterface::SendSensorMessages(int time_usec, Eigen::Vector3d accel_b, Eigen::Vector3d gyro_b) {

  mavlink_hil_sensor_t sensor_msg;

  sensor_msg.time_usec = time_usec;
  sensor_msg.xacc = accel_b[0];
  sensor_msg.yacc = accel_b[1];
  sensor_msg.zacc = accel_b[2];
  sensor_msg.xgyro = gyro_b[0];
  sensor_msg.ygyro = gyro_b[1];
  sensor_msg.zgyro = gyro_b[2];

  sensor_msg.fields_updated = (uint16_t)SensorSource::ACCEL | (uint16_t)SensorSource::GYRO;

  // send only mag data
  if (mag_updated_) {
    sensor_msg.xmag = mag_b_[0];
    sensor_msg.ymag = mag_b_[1];
    sensor_msg.zmag = mag_b_[2];
    sensor_msg.fields_updated = sensor_msg.fields_updated | (uint16_t)SensorSource::MAG;

    mag_updated_ = false;
  }

  // send only baro data
  if (baro_updated_) {
    sensor_msg.temperature = temperature_;
    sensor_msg.abs_pressure = abs_pressure_;
    sensor_msg.pressure_alt = pressure_alt_;
    sensor_msg.fields_updated = sensor_msg.fields_updated | (uint16_t)SensorSource::BARO;

    baro_updated_ = false;
  }

  // send only diff pressure data
  if (diff_press_updated_) {
    sensor_msg.diff_pressure = diff_pressure_;
    sensor_msg.fields_updated = sensor_msg.fields_updated | (uint16_t)SensorSource::DIFF_PRESS;

    diff_press_updated_ = false;
  }

  if (!hil_mode_ || (hil_mode_ && !hil_state_level_)) {
    mavlink_message_t msg;
    mavlink_msg_hil_sensor_encode_chan(1, 200, MAVLINK_COMM_0, &msg, &sensor_msg);
    send_mavlink_message(&msg);
  }
}

void MavlinkInterface::UpdateBarometer(double temperature, double abs_pressure, double pressure_alt) {
  temperature_ = temperature;
  abs_pressure_ = abs_pressure;
  pressure_alt_ = pressure_alt;

  baro_updated_ = true;
}

void MavlinkInterface::UpdateDiffpressure(double diff_pressure) {
  diff_pressure_ = diff_pressure;

  diff_press_updated_ = true;
}

void MavlinkInterface::UpdateMag(Eigen::Vector3d mag_b) {
  mag_b_ = mag_b;

  mag_updated_ = true;
}

void MavlinkInterface::pollForMAVLinkMessages()
{
  if (gotSigInt_) {
    return;
  }

  bool received_actuator = false;

  do {
    int timeout_ms = (received_first_actuator_ && enable_lockstep_) ? 1000 : 0;
    int ret = ::poll(&fds_[0], N_FDS, timeout_ms);

    if (ret < 0) {
      std::cerr << "poll error: " << strerror(errno) << "\n";
      return;
    }

    if (ret == 0 && timeout_ms > 0) {
      std::cerr << "poll timeout\n";
      return;
    }

    for (int i = 0; i < N_FDS; i++) {
      if(fds_[i].revents == 0) {
        continue;
      }

      if (!(fds_[i].revents & POLLIN)) {
        continue;
      }

      if (i == LISTEN_FD) { // if event is raised on the listening socket
        acceptConnections();
      } else { // if event is raised on connection socket
        int ret = recvfrom(fds_[i].fd, _buf, sizeof(_buf), 0, (struct sockaddr *)&remote_simulator_addr_, &remote_simulator_addr_len_);
        if (ret < 0) {
          // all data is read if EWOULDBLOCK is raised
          if (errno != EWOULDBLOCK) { // disconnected from client
            std::cerr << "recvfrom error: " << strerror(errno) << "\n";
          }
          continue;
        }

        // client closed the connection orderly, only makes sense on tcp
        if (use_tcp_ && ret == 0) {
          std::cerr << "Connection closed by client." << "\n";
          close_conn_ = true;
          continue;
        }

        // data received
        int len = ret;
        mavlink_message_t msg;
        mavlink_status_t status;
        for (unsigned i = 0; i < len; ++i) {
          if (mavlink_parse_char(MAVLINK_COMM_0, _buf[i], &msg, &status)) {
            if (hil_mode_) {
              send_mavlink_message(&msg);
            }
            handle_message(&msg, received_actuator);
          }
        }
      }
    }
  } while (!close_conn_ && received_first_actuator_ && !received_actuator && enable_lockstep_ && !gotSigInt_);
}

void MavlinkInterface::pollFromQgcAndSdk()
{
  struct pollfd fds[2] = {};
  fds[0].fd = qgc_socket_fd_;
  fds[0].events = POLLIN;
  fds[1].fd = sdk_socket_fd_;
  fds[1].events = POLLIN;

  const int timeout_ms = 0;

  int ret = ::poll(&fds[0], 2, timeout_ms);

  if (ret < 0) {
    std::cerr << "poll error: " << strerror(errno) << "\n";
    return;
  }

  if (fds[0].revents & POLLIN) {
    int len = recvfrom(qgc_socket_fd_, _buf, sizeof(_buf), 0, (struct sockaddr *)&remote_qgc_addr_, &remote_qgc_addr_len_);

    if (len > 0) {
      mavlink_message_t msg;
      mavlink_status_t status;
      for (unsigned i = 0; i < len; ++i) {
        if (mavlink_parse_char(MAVLINK_COMM_1, _buf[i], &msg, &status)) {
          // forward message from QGC to serial
          send_mavlink_message(&msg);
        }
      }
    }
  }

  if (fds[1].revents & POLLIN) {
    int len = recvfrom(sdk_socket_fd_, _buf, sizeof(_buf), 0, (struct sockaddr *)&remote_sdk_addr_, &remote_sdk_addr_len_);

    if (len > 0) {
      mavlink_message_t msg;
      mavlink_status_t status;
      for (unsigned i = 0; i < len; ++i) {
        if (mavlink_parse_char(MAVLINK_COMM_2, _buf[i], &msg, &status)) {
          // forward message from SDK to serial
          send_mavlink_message(&msg);
        }
      }
    }
  }
}

void MavlinkInterface::acceptConnections()
{
  if (fds_[CONNECTION_FD].fd > 0) {
    return;
  }

  // accepting incoming connections on listen fd
  int ret =
    accept(fds_[LISTEN_FD].fd, (struct sockaddr *)&remote_simulator_addr_, &remote_simulator_addr_len_);

  if (ret < 0) {
    if (errno != EWOULDBLOCK) {
      std::cerr << "accept error: " << strerror(errno) << "\n";
    }
    return;
  }

  // assign socket to connection descriptor on success
  fds_[CONNECTION_FD].fd = ret; // socket is replaced with latest connection
  fds_[CONNECTION_FD].events = POLLIN | POLLOUT; // read/write
}

void MavlinkInterface::handle_message(mavlink_message_t *msg, bool &received_actuator)
{
  switch (msg->msgid) {
  case MAVLINK_MSG_ID_HIL_ACTUATOR_CONTROLS:
    const std::lock_guard<std::mutex> lock(actuator_mutex);

    mavlink_hil_actuator_controls_t controls;
    mavlink_msg_hil_actuator_controls_decode(msg, &controls);

    armed_ = (controls.mode & MAV_MODE_FLAG_SAFETY_ARMED);

    for (unsigned i = 0; i < n_out_max; i++) {
      input_index_[i] = i;
    }

    // set rotor speeds, controller targets
    input_reference_.resize(n_out_max);
    for (int i = 0; i < input_reference_.size(); i++) {
      input_reference_[i] = controls.controls[i];
    }

    received_actuator = true;
    received_first_actuator_ = true;
    break;
  }
}

void MavlinkInterface::forward_mavlink_message(const mavlink_message_t *message)
{
  if (gotSigInt_) {
    return;
  }

  uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
  int packetlen = mavlink_msg_to_send_buffer(buffer, message);
  ssize_t len;
  if (qgc_socket_fd_ > 0) {
    len = sendto(qgc_socket_fd_, buffer, packetlen, 0, (struct sockaddr *)&remote_qgc_addr_, remote_qgc_addr_len_);

    if (len <= 0)
    {
      std::cerr << "Failed sending mavlink message to QGC: " << strerror(errno) << "\n";
    }
  }

  if (sdk_socket_fd_ > 0) {
    len = sendto(sdk_socket_fd_, buffer, packetlen, 0, (struct sockaddr *)&remote_sdk_addr_, remote_sdk_addr_len_);
    if (len <= 0)
    {
      std::cerr << "Failed sending mavlink message to SDK: " << strerror(errno) << "\n";
    }
  }
}

void MavlinkInterface::send_mavlink_message(const mavlink_message_t *message)
{
  assert(message != nullptr);

  if (gotSigInt_ || close_conn_) {
    return;
  }

  if (serial_enabled_) {

    if (!is_open()) {
      std::cerr << "Serial port closed! \n";
      return;
    }

    {
      std::lock_guard<std::recursive_mutex> lock(mutex);

      if (tx_q.size() >= MAX_TXQ_SIZE) {
        std::cout << "Tx queue overflow\n";
      }
      tx_q.emplace_back(message);
    }
    io_service.post(std::bind(&MavlinkInterface::do_write, this, true));

  } else {
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    int packetlen = mavlink_msg_to_send_buffer(buffer, message);

    if (fds_[CONNECTION_FD].fd > 0) {
      int timeout_ms = (received_first_actuator_ && enable_lockstep_) ? 1000 : 0;
      int ret = ::poll(&fds_[0], N_FDS, timeout_ms);

      if (ret < 0) {
        std::cerr << "poll error: " << strerror(errno) << "\n";
        return;
      }

      if (ret == 0 && timeout_ms > 0) {
        std::cerr << "poll timeout\n";
        return;
      }

      if (!(fds_[CONNECTION_FD].revents & POLLOUT)) {
        std::cerr << "invalid events at fd:" << fds_[CONNECTION_FD].revents << "\n";
        return;
      }

      size_t len;
      if (use_tcp_) {
        len = send(fds_[CONNECTION_FD].fd, buffer, packetlen, 0);
      } else {
        len = sendto(fds_[CONNECTION_FD].fd, buffer, packetlen, 0, (struct sockaddr *)&remote_simulator_addr_, remote_simulator_addr_len_);
      }
      if (len < 0) {
        std::cerr << "Failed sending mavlink message: " << strerror(errno) << "\n";
        if (errno == ECONNRESET || errno == EPIPE) {
          if (use_tcp_) { // udp socket remains alive
            std::cerr << "Closing connection." << "\n";
            close_conn_ = true;
          }
        }
      }
    }
  }
}

void MavlinkInterface::open() {
  try{
    serial_dev.open(device_);
    serial_dev.set_option(boost::asio::serial_port_base::baud_rate(baudrate_));
    serial_dev.set_option(boost::asio::serial_port_base::character_size(8));
    serial_dev.set_option(boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none));
    serial_dev.set_option(boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one));
    serial_dev.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none));
    std::cout << "Opened serial device " << device_ << "\n";
  }
  catch (boost::system::system_error &err) {
    std::cerr <<"Error opening serial device: " << err.what() << "\n";
  }
}

void MavlinkInterface::close()
{
  if(serial_enabled_) {
    ::close(qgc_socket_fd_);
    ::close(sdk_socket_fd_);

    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!is_open())
      return;

    io_service.stop();
    serial_dev.close();

    if (io_thread.joinable())
      io_thread.join();

  } else {

    ::close(fds_[CONNECTION_FD].fd);
    fds_[CONNECTION_FD] = { 0, 0, 0 };
    fds_[CONNECTION_FD].fd = -1;

    received_first_actuator_ = false;

  }
}

void MavlinkInterface::do_write(bool check_tx_state){
  if (check_tx_state && tx_in_progress)
    return;

  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (tx_q.empty())
    return;

  tx_in_progress = true;
  auto &buf_ref = tx_q.front();

  serial_dev.async_write_some(
    boost::asio::buffer(buf_ref.dpos(), buf_ref.nbytes()), [this, &buf_ref] (boost::system::error_code error,   size_t bytes_transferred)
    {
      assert(bytes_transferred <= buf_ref.len);
      if(error) {
        std::cerr << "Serial error: " << error.message() << "\n";
      return;
      }

    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (tx_q.empty()) {
      tx_in_progress = false;
      return;
    }

    buf_ref.pos += bytes_transferred;
    if (buf_ref.nbytes() == 0) {
      tx_q.pop_front();
    }

    if (!tx_q.empty()) {
      do_write(false);
    }
    else {
      tx_in_progress = false;
    }
  });
}

void MavlinkInterface::do_read(void)
{
  serial_dev.async_read_some(boost::asio::buffer(rx_buf), boost::bind(
      &MavlinkInterface::parse_buffer, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred
      )
  );
}

// Based on MAVConnInterface::parse_buffer in MAVROS
void MavlinkInterface::parse_buffer(const boost::system::error_code& err, std::size_t bytes_t){
  mavlink_status_t status;
  mavlink_message_t message;
  uint8_t *buf = this->rx_buf.data();

  assert(rx_buf.size() >= bytes_t);

  for(; bytes_t > 0; bytes_t--)
  {
    auto c = *buf++;

    auto msg_received = static_cast<Framing>(mavlink_frame_char_buffer(&m_buffer, &m_status, c, &message, &status));
    if (msg_received == Framing::bad_crc || msg_received == Framing::bad_signature) {
      _mav_parse_error(&m_status);
      m_status.msg_received = MAVLINK_FRAMING_INCOMPLETE;
      m_status.parse_state = MAVLINK_PARSE_STATE_IDLE;
      if (c == MAVLINK_STX) {
        m_status.parse_state = MAVLINK_PARSE_STATE_GOT_STX;
        m_buffer.len = 0;
        mavlink_start_checksum(&m_buffer);
      }
    }

    if (msg_received != Framing::incomplete) {
      if (hil_mode_) {
        forward_mavlink_message(&message);
      }
      bool not_used;
      handle_message(&message, not_used);
    }
  }
  do_read();
}

void MavlinkInterface::onSigInt() {
  gotSigInt_ = true;
  close();
}

Eigen::VectorXd MavlinkInterface::GetActuatorControls() {
  const std::lock_guard<std::mutex> lock(actuator_mutex);
  return input_reference_;
}

bool MavlinkInterface::GetReceivedFirstActuator() {
  return received_first_actuator_;
}

bool MavlinkInterface::GetArmedState() {
  const std::lock_guard<std::mutex> lock(actuator_mutex);
  return armed_;
}

void MavlinkInterface::SetSerialEnabled(bool serial_enabled) {
  serial_enabled_ = serial_enabled;
}

void MavlinkInterface::SetUseTcp(bool use_tcp) {
  use_tcp_ = use_tcp;
}

void MavlinkInterface::SetEnableLockstep(bool enable_lockstep) {
  enable_lockstep_ = enable_lockstep;
}

void MavlinkInterface::SetMavlinkAddr(std::string mavlink_addr) {
  mavlink_addr_str_ = mavlink_addr;
}

void MavlinkInterface::SetMavlinkTcpPort(int mavlink_tcp_port) {
  mavlink_tcp_port_ = mavlink_tcp_port;
}

void MavlinkInterface::SetMavlinkUdpPort(int mavlink_udp_port) {
  mavlink_udp_port_ = mavlink_udp_port;
}

void MavlinkInterface::SetQgcAddr(std::string qgc_addr) {
  qgc_addr_ = qgc_addr;
}

void MavlinkInterface::SetQgcUdpPort(int qgc_udp_port) {
  qgc_udp_port_ = qgc_udp_port;
}

void MavlinkInterface::SetSdkAddr(std::string sdk_addr) {
  sdk_addr_ = sdk_addr;
}

void MavlinkInterface::SetSdkUdpPort(int sdk_udp_port) {
  sdk_udp_port_ = sdk_udp_port;
}

void MavlinkInterface::SetHILMode(bool hil_mode) {
  hil_mode_ = hil_mode;
}

void MavlinkInterface::SetHILStateLevel(bool hil_state_level) {
  hil_state_level_ = hil_state_level;
}