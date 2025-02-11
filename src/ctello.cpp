//  CTello is a C++ library to interact with the DJI Ryze Tello Drone
//  Copyright (C) 2020 Carlos Perez-Lopez
//
//  This library is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>
//
//  You can contact the author via carlospzlz@gmail.com

#include "ctello.h"

namespace
{

// Binds the given socket file descriptor ot the given port.
// Returns whether it suceeds or not and the error message.
std::pair<bool, std::string> BindSocketToPort(const int sockfd, const int port)
{
    sockaddr_in listen_addr{};
    // htons converts from host byte order to network byte order.
    listen_addr.sin_port = htons(port);
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_family = AF_INET;
    int result = bind(sockfd, reinterpret_cast<sockaddr*>(&listen_addr),
                      sizeof(listen_addr));

    if (result == -1)
    {
        std::stringstream ss;
        ss << "bind to " << port << ": " << errno;
        ss << " (" << strerror(errno) << ")";
        return {false, ss.str()};
    }

    return {true, ""};
}

// Finds the socket address given an ip and a port.
// Returns whether it suceeds or not and the error message.
std::pair<bool, std::string> FindSocketAddr(const char* const ip,
                                            const char* const port,
                                            sockaddr_storage* const addr)
{
    addrinfo* result_list{nullptr};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int result = getaddrinfo(ip, port, &hints, &result_list);

    if (result != 0)
    {
        std::stringstream ss;
        ss << "getaddrinfo: " << result;
        ss << " (" << gai_strerror(result) << ") ";
        return {false, ss.str()};
    }

    memcpy(addr, result_list->ai_addr, result_list->ai_addrlen);
    freeaddrinfo(result_list);

    return {true, ""};
}

// Sends a string of bytes to the given destination address.
// Returns the number of sent bytes and, if -1, the error message.
std::pair<int, std::string> SendTo(const int sockfd,
                                   sockaddr_storage& dest_addr,
                                   const std::vector<unsigned char>& message)
{
    const socklen_t addr_len{sizeof(dest_addr)};
    int result = sendto(sockfd, (char*)message.data(), message.size(), 0,
                        reinterpret_cast<sockaddr*>(&dest_addr), addr_len);
    std::stringstream ss;
    if (result == -1)
    {
        ss << "sendto: " << errno;
        ss << " (" << strerror(errno) << ")";
        return {-1, ss.str()};
    }
    return {result, ""};
}

// Receives a text response from the given destination address.
// Returns the number of received bytes and, if -1, the error message.
#if defined(__linux__)
std::pair<int, std::string> ReceiveFrom(const int sockfd,
                                        sockaddr_storage& addr,
                                        std::vector<unsigned char>& buffer,
                                        const int buffer_size = 1024,
                                        const int flags = MSG_DONTWAIT)
#elif defined(_WIN32)
std::pair<int, std::string> ReceiveFrom(const int sockfd,
                                        sockaddr_storage& addr,
                                        std::vector<unsigned char>& buffer,
                                        const int buffer_size = 1024)
#endif
{
    socklen_t addr_len{sizeof(addr)};
    buffer.resize(buffer_size, '\0');
    // MSG_DONTWAIT -> Non-blocking
    // recvfrom is storing (re-populating) the sender address in addr.
#if defined(_WIN32)
    int result = recvfrom(sockfd, (char*)buffer.data(), buffer_size, 0,
                          reinterpret_cast<sockaddr*>(&addr), &addr_len);
#elif defined(__linux__)
    int result = recvfrom(sockfd, buffer.data(), buffer_size, flags,
                          reinterpret_cast<sockaddr*>(&addr), &addr_len);
#endif
    if (result == -1)
    {
        std::stringstream ss;
        ss << "recvfrom: " << errno;
        ss << " (" << strerror(errno) << ")";
        return {-1, ss.str()};
    }

    return {result, ""};
}
}  // namespace

namespace ctello
{
Tello::Tello()
{
#if defined(_WIN32)
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Failed. Error Code : %d", WSAGetLastError());
        exit(1);
    }
#endif
    withThread = false;
    createSockets();
    Bind();
}
Tello::Tello(bool withThreads)
{
#if defined(_WIN32)
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Failed. Error Code : %d", WSAGetLastError());
        exit(1);
    }
#endif
    withThread = true;
    createSockets();
    responses = std::vector<std::string>{};
    BindWithOutStatus();
    responseReceiver = std::thread(&Tello::listenToResponses, this);
    stateReceiver = std::thread(&Tello::listenToState, this);
    std::cout << "Finding Tello ..." << std::endl;
    SendCommandWithResponse("command");
}
void Tello::listenToState()
{
    while (m_state_sockfd)
    {
        try
        {
            auto response = GetState();
            if (response && response.has_value())
            {
                auto startYawPosition = response.value().find("yaw:") + 4;
                auto endYawPosition =
                    response.value().substr(startYawPosition).find(';');

                yaw = std::stoi(
                    response.value().substr(startYawPosition, endYawPosition));
                std::string stringFromYawPosition =
                    response.value().substr(endYawPosition);
                auto startHeightPosition =
                    stringFromYawPosition.find("tof:") + 4;
                auto endHeightPosition =
                    stringFromYawPosition.substr(startHeightPosition).find(';');

                height = std::stoi(stringFromYawPosition.substr(
                    startHeightPosition, endHeightPosition));
                std::string stringFromHeight =
                    stringFromYawPosition.substr(endHeightPosition);
                auto startBatteryPosition = stringFromHeight.find("bat:") + 4;
                auto endBatteryPosition =
                    stringFromHeight.substr(startBatteryPosition).find(';');
                battery = std::stoi(stringFromHeight.substr(
                    startBatteryPosition, endBatteryPosition));
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        catch (...)
        {
            std::cerr << "something happend at listenToState" << std::endl;
        }
    }
}
void Tello::listenToResponses()
{
    while (m_command_sockfd)
    {
        try
        {
            auto response = ReceiveResponse();
            if (response && response.has_value())
            {
                std::string answer(response.value());
                responses.emplace_back(answer);
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        catch (...)
        {
            std::cerr << "something happend at listenToResponses" << std::endl;
        }
    }
}
Tello::~Tello()
{
    closeSockets();
    /*if (!logFileName.empty()){
        auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::string date = std::ctime(&now);
        telloLogFile << "end time:" << date << std::endl;
        telloLogFile.close();
    }*/
}
void Tello::closeSockets()
{
#if defined(_WIN32)
    closesocket(m_command_sockfd);
    closesocket(m_state_sockfd);
#elif defined(__linux__)
    close(m_command_sockfd);
    close(m_state_sockfd);
#endif
    m_command_sockfd = 0;
    m_state_sockfd = 0;
    if (withThread)
    {
        responseReceiver.join();
        stateReceiver.join();
    }
}
void Tello::createSockets()
{
    m_command_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    m_state_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
#if defined(_WIN32)

    optval = 1;
    ioctlsocket(m_command_sockfd, FIONBIO, &optval);
#endif
    optval = 1;
    setsockopt(m_command_sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval,
               sizeof(int));
    setsockopt(m_state_sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval,
               sizeof(int));
}
bool Tello::BindWithOutStatus(const int local_client_command_port,
                              int local_server_command_port)
{
    // UDP Client to send commands and receive responses
    auto result =
        ::BindSocketToPort(m_command_sockfd, local_client_command_port);
    if (!result.first)
    {
        std::cerr << result.second << std::endl;
        return false;
    }
    m_local_client_command_port = local_client_command_port;
    result = ::FindSocketAddr(TELLO_SERVER_IP, TELLO_SERVER_COMMAND_PORT,
                              &m_tello_server_command_addr);
    if (!result.first)
    {
        std::cerr << result.second << std::endl;
        return false;
    }

    // Local UDP Server to listen for the Tello Status
    result = ::BindSocketToPort(m_state_sockfd, local_server_command_port);
    if (!result.first)
    {
        std::cerr << result.second << std::endl;
        return false;
    }

    return true;
}
bool Tello::Bind(const int local_client_command_port,
                 int local_server_command_port)
{
    // UDP Client to send commands and receive responses
    auto result =
        ::BindSocketToPort(m_command_sockfd, local_client_command_port);
    if (!result.first)
    {
        std::cerr << result.second << std::endl;
        return false;
    }
    m_local_client_command_port = local_client_command_port;
    result = ::FindSocketAddr(TELLO_SERVER_IP, TELLO_SERVER_COMMAND_PORT,
                              &m_tello_server_command_addr);
    if (!result.first)
    {
        std::cerr << result.second << std::endl;
        return false;
    }

    // Local UDP Server to listen for the Tello Status
    result = ::BindSocketToPort(m_state_sockfd, local_server_command_port);
    if (!result.first)
    {
        std::cerr << result.second << std::endl;
        return false;
    }

    // Finding Tello
    std::cout << "Finding Tello ..." << std::endl;
    FindTello();
    std::cout << "Entered SDK mode" << std::endl;

    ShowTelloInfo();
    return true;
}

void Tello::FindTello()
{
    do
    {
        SendCommand("command");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (!(ReceiveResponse()));
}
std::string Tello::GetTelloName()
{
    std::optional<std::string> response;
    SendCommand("sn?");
    while (!(response = ReceiveResponse()))
        ;
    return response.value();
}
bool Tello::EasyLanding()
{
    while (!SendCommandWithResponseByThread("down 20"))
        ;
    return SendCommandWithResponse("land");
}
double Tello::GetSpeedStatus()
{
    std::optional<std::string> response;
    SendCommand("speed?");
    while (!(response = ReceiveResponse()))
        ;
    if (response)
    {
        try
        {
            return std::stod(response.value());
        }
        catch (...)
        {
            return 0;
        }
    }
    return 0;
}
std::string Tello::GetAccelerationStatus()
{
    std::optional<std::string> response;
    SendCommand("acceleration?");
    while (!(response = ReceiveResponse()))
        ;
    if (response.has_value())
    {
        return response.value();
    }
    return "";
}
int Tello::GetHeightStatus()
{
    std::optional<std::string> response;
    SendCommand("height?");
    while (!(response = ReceiveResponse()))
        ;
    if (response)
    {
        try
        {
            return std::stoi(response.value());
        }
        catch (...)
        {
            return 0;
        }
    }
    return 0;
}
int Tello::GetBatteryStatus()
{
    std::optional<std::string> response;
    SendCommand("battery?");
    while (!(response = ReceiveResponse()))
        ;
    if (response)
    {
        try
        {
            return std::stoi(response.value());
        }
        catch (...)
        {
            return 0;
        }
    }
    return 0;
}
void Tello::ShowTelloInfo()
{
    std::optional<std::string> response;

    SendCommand("sn?");
    while (!(response = ReceiveResponse()))
        ;
    std::cout << "Serial Number: " << *response << std::endl;

    SendCommand("sdk?");
    while (!(response = ReceiveResponse()))
        ;
    std::cout << "Tello SDK: " << *response << std::endl;

    SendCommand("wifi?");
    while (!(response = ReceiveResponse()))
        ;
    std::cout << "Wi-Fi Signal: " << *response << std::endl;

    SendCommand("battery?");
    while (!(response = ReceiveResponse()))
        ;
    std::cout << "Battery: " << *response << std::endl;
}
bool Tello::SendCommand(const std::string& command)
{
    const std::vector<unsigned char> message{command.begin(), command.end()};
    const auto result =
        ::SendTo(m_command_sockfd, m_tello_server_command_addr, message);
    const int bytes{result.first};
    if (bytes == -1)
    {
        std::cerr << result.second << std::endl;
        return false;
    }
    return true;
}
bool Tello::SendCommandWithResponse(const std::string& command,
                                    int amountOfTries)
{
    const std::vector<unsigned char> message{command.begin(), command.end()};
    const auto result =
        ::SendTo(m_command_sockfd, m_tello_server_command_addr, message);
    const int bytes{result.first};
    if (bytes == -1)
    {
        std::cerr << result.second << std::endl;
        return false;
    }
    std::optional<std::string> response = ReceiveResponse();
    while (!response || !response.has_value())
    {
        if (amountOfTries > 0)
        {
            amountOfTries -= 1;
        }
        else
        {
            return false;
        }
        response = ReceiveResponse();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    std::string strAnswer(response.value());
    std::cout << strAnswer + ": " + std::string(message.begin(), message.end())
              << std::endl;
    bool isSuccess = strAnswer.find("ok") != std::string::npos ||
                     strAnswer.find("OK") != std::string::npos;
    return isSuccess;
}
bool Tello::SendCommandWithResponseByThread(const std::string& command,
                                            int amountOfTries)
{
    const std::vector<unsigned char> message{command.begin(), command.end()};
    const auto result =
        ::SendTo(m_command_sockfd, m_tello_server_command_addr, message);
    const int bytes{result.first};
    if (bytes == -1)
    {
        std::cerr << result.second << std::endl;
        return false;
    }
    std::string strAnswer;
    do
    {
        if (amountOfTries > 0)
        {
            amountOfTries -= 1;
        }
        else
        {
            std::cout << "no response: " +
                             std::string(message.begin(), message.end())
                      << std::endl;
            return false;
        }
        if (responses.empty())
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        strAnswer = responses.back();
        break;
    } while (strAnswer.empty());

    responses.pop_back();
    std::cout << strAnswer + ": " + std::string(message.begin(), message.end())
              << std::endl;
    bool isSuccess = strAnswer.find("ok") != std::string::npos;
    return isSuccess;
}

std::optional<std::string> Tello::ReceiveResponse()
{
    const int size{32};
    std::vector<unsigned char> buffer(size, '\0');
    const auto result = ::ReceiveFrom(
        m_command_sockfd, m_tello_server_command_addr, buffer, size);
    const int bytes{result.first};
    if (bytes < 1)
    {
        return {};
    }
    std::string response{buffer.begin(), buffer.end()};
    // Some responses contain trailing white spaces.
    response.erase(response.find_last_not_of(" \n\r\t") + 1);
    response = response.substr(0,response.find_last_not_of('\0')+1);
    return response;
}
int Tello::GetBatteryState(int amountOfTries)
{
    std::optional<std::string> response;
    while (!response.has_value() && amountOfTries-- != 0)
    {
        response = GetState();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (amountOfTries <= 0)
    {
        std::cout << "cant get state" << std::endl;
        return 100;
    }
    auto startBatteryPosition = response.value().find("bat:") + 4;
    auto endBatteryPosition =
        response.value().substr(startBatteryPosition).find(';');
    int Battery = std::stoi(
        response.value().substr(startBatteryPosition, endBatteryPosition));
    return Battery;
}
int Tello::GetYawState(int amountOfTries)
{
    std::optional<std::string> response;
    while (!response.has_value() && amountOfTries-- != 0)
    {
        response = GetState();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (amountOfTries <= 0)
    {
        std::cout << "cant get state" << std::endl;
        return 100;
    }
    auto startBatteryPosition = response.value().find("yaw:") + 4;
    auto endBatteryPosition =
        response.value().substr(startBatteryPosition).find(';');
    int Battery = std::stoi(
        response.value().substr(startBatteryPosition, endBatteryPosition));
    return Battery;
}
int Tello::GetHeightState(int amountOfTries)
{
    std::optional<std::string> response;
    while (!response.has_value() && amountOfTries-- != 0)
    {
        response = GetState();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (amountOfTries <= 0)
    {
        std::cout << "cant get state" << std::endl;
        return 0;
    }
    auto startHeightPosition = response.value().find("tof:") + 4;
    auto endHeightPosition =
        response.value().substr(startHeightPosition).find(';');
    int height = std::stoi(
        response.value().substr(startHeightPosition, endHeightPosition));
    return height;
}
std::optional<std::string> Tello::GetState()
{
    sockaddr_storage addr;
    const int size{1024};
    std::vector<unsigned char> buffer(size, '\0');
    const auto result = ::ReceiveFrom(m_state_sockfd, addr, buffer, size);
    const int bytes{result.first};
    if (bytes < 1)
    {
        return {};
    }
    std::string response{buffer.begin(), buffer.end()};
    // Some responses contain trailing white spaces.
    response.erase(response.find_last_not_of(" \n\r\t") + 1);
    return response;
}
}  // namespace ctello