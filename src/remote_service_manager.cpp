// Copyright (c) 2018 Baidu, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "remote_service_manager.h"
#include <curl/curl.h>
#include <cstdio>
#include <string>
#include "app_log.h"
#include "file_watcher.h"
#include "rapidjson.h"
#include "thread_data_base.h"

namespace dmkit {

RemoteServiceManager::RemoteServiceManager() {
}

RemoteServiceManager::~RemoteServiceManager() {
    FileWatcher::get_instance().unregister_file(this->_conf_file_path);
    this->_p_channel_map.reset();
}

static inline void destroy_channel_map(ChannelMap* p) {
    APP_LOG(TRACE) << "Destroying service map...";
    if (nullptr == p) {
        return;
    }
    for (auto& channel : *p) {
        if (nullptr != channel.second.channel) {
            delete channel.second.channel;
            channel.second.channel = nullptr;
            APP_LOG(TRACE) << "Destroyed service" << channel.first;
        }
    }
    delete p;
}

int RemoteServiceManager::init(const char* path, const char* conf) {
    std::string file_path;
    if (path != nullptr) {
        file_path += path;
    }
    if (!file_path.empty() && file_path[file_path.length() - 1] != '/') {
        file_path += '/';
    }
    if (conf != nullptr) {
        file_path += conf;
    }
    this->_conf_file_path = file_path;

    ChannelMap* channel_map = this->load_channel_map();
    if (channel_map == nullptr) {
        APP_LOG(ERROR) << "Failed to init RemoteServiceManager, cannot load channel map";
        return -1;
    }

    this->_p_channel_map.reset(channel_map,
                               [](ChannelMap* p) { destroy_channel_map(p); });

    FileWatcher::get_instance().register_file(
        this->_conf_file_path, RemoteServiceManager::service_conf_change_callback, this, true);

    return 0;
}

int RemoteServiceManager::reload() {
    APP_LOG(TRACE) << "Reloading RemoteServiceManager...";
    ChannelMap* channel_map = this->load_channel_map();
    if (channel_map == nullptr) {
        APP_LOG(ERROR) << "Failed to reload RemoteServiceManager, cannot load channel map";
        return -1;
    }

    this->_p_channel_map.reset(channel_map,
                               [](ChannelMap* p) { destroy_channel_map(p); });
    APP_LOG(TRACE) << "Reload finished.";
    return 0;
}

int RemoteServiceManager::service_conf_change_callback(void* param) {
    RemoteServiceManager* rsm = (RemoteServiceManager*)param;
    return rsm->reload();
}

int RemoteServiceManager::call(const std::string& service_name,
                               const RemoteServiceParam& params,
                               RemoteServiceResult& result) const {
    std::shared_ptr<ChannelMap> p_channel_map(this->_p_channel_map);
    if (p_channel_map == nullptr) {
        APP_LOG(ERROR) << "Remote service call failed, channel map is null";
        return -1;
    }

    if (p_channel_map->find(service_name) == p_channel_map->end()) {
        APP_LOG(ERROR) << "Remote service call failed, cannot find service " << service_name;
        return -1;
    }

    RemoteServiceChannel& service_channel = (*p_channel_map)[service_name];

    APP_LOG(TRACE) << "Calling service " << service_name;

    int ret = 0;
    std::string remote_side;
    int latency = 0;
    if (service_channel.protocol == "http") {
        if (service_channel.channel != nullptr) {
            ret = this->call_http_by_BRPC_NAMESPACE(service_channel.channel,
                                          params.url,
                                          params.http_method,
                                          service_channel.headers,
                                          params.payload,
                                          result.result,
                                          remote_side,
                                          latency);
        } else {
            ret = this->call_http_by_curl(params.url,
                                          params.http_method,
                                          service_channel.headers,
                                          params.payload,
                                          service_channel.timeout_ms,
                                          service_channel.max_retry,
                                          result.result,
                                          remote_side,
                                          latency);
        }
    } else {
        APP_LOG(ERROR) << "Remote service call failed. Unknown protocol" << service_channel.protocol;
        ret = -1;
    }
    std::string log_str;
    log_str += "remote:";
    log_str += remote_side;
    log_str += "|tm:";
    log_str += std::to_string(latency);
    log_str += "|ret:";
    log_str += std::to_string(ret);
    std::string log_key = "service_";
    log_key += service_name;

    APP_LOG(TRACE) << "remote_side=" << remote_side << ", cost=" << latency;
    ThreadDataBase* tls = static_cast<ThreadDataBase*>(BRPC_NAMESPACE::thread_local_data());
    // All backend requests are logged.
    tls->add_notice_log(log_key, log_str);

    return ret;
}

ChannelMap* RemoteServiceManager::load_channel_map() {
    APP_LOG(TRACE) << "Loading channel map...";
    FILE* fp = fopen(this->_conf_file_path.c_str(), "r");
    if (fp == nullptr) {
        APP_LOG(ERROR) << "Failed to open file " << this->_conf_file_path;
        return nullptr;
    }
    char read_buffer[1024];
    rapidjson::FileReadStream is(fp, read_buffer, sizeof(read_buffer));
    rapidjson::Document doc;
    doc.ParseStream(is);
    fclose(fp);

    if (doc.HasParseError() || !doc.IsObject()) {
        APP_LOG(ERROR) << "Failed to parse RemoteServiceManager settings";
        return nullptr;
    }

    ChannelMap* channel_map = new ChannelMap();
    rapidjson::Value::ConstMemberIterator service_iter;
    for (service_iter = doc.MemberBegin(); service_iter != doc.MemberEnd(); ++service_iter) {
        // Service name as the key for the channel.
        std::string service_name = service_iter->name.GetString();
        if (!service_iter->value.IsObject()) {
            APP_LOG(ERROR) << "Invalid service settings for " << service_name
                << ", expecting type object for service setting.";
            destroy_channel_map(channel_map);
            return nullptr;
        }
        const rapidjson::Value& settings = service_iter->value;
        rapidjson::Value::ConstMemberIterator setting_iter;
        // Naming service url such as https://www.baidu.com.
        // All supported url format can be found in BRPC docs.
        setting_iter = settings.FindMember("naming_service_url");
        if (setting_iter == settings.MemberEnd() || !setting_iter->value.IsString()) {
            APP_LOG(ERROR) << "Invalid service settings for " << service_name
                << ", expecting type String for property naming_service_url.";
            destroy_channel_map(channel_map);
            return nullptr;
        }
        std::string naming_service_url = setting_iter->value.GetString();
        // Load balancer name such as random, rr.
        // All supported balancer can be found in BRPC docs.
        setting_iter = settings.FindMember("load_balancer_name");
        if (setting_iter == settings.MemberEnd() || !setting_iter->value.IsString()) {
            APP_LOG(ERROR) << "Invalid service settings for " << service_name
                << ", expecting type String for property load_balancer_name.";
            destroy_channel_map(channel_map);
            return nullptr;
        }
        std::string load_balancer_name = setting_iter->value.GetString();
        // Protocol for the channel.
        // Currently we support http.
        setting_iter = settings.FindMember("protocol");
        if (setting_iter == settings.MemberEnd() || !setting_iter->value.IsString()) {
            APP_LOG(ERROR) << "Invalid service settings for " << service_name
                << ", expecting type String for property protocol.";
            destroy_channel_map(channel_map);
            return nullptr;
        }
        std::string protocol = setting_iter->value.GetString();
        // Client to use for sending request.
        setting_iter = settings.FindMember("client");
        std::string client;
        if (setting_iter != settings.MemberEnd() && setting_iter->value.IsString()) {
            client = setting_iter->value.GetString();
        }
        // Timeout value in millisecond.
        setting_iter = settings.FindMember("timeout_ms");
        if (setting_iter == settings.MemberEnd() || !setting_iter->value.IsInt()) {
            APP_LOG(ERROR) << "Invalid service settings for " << service_name
                << ", expecting type Int for property timeout_ms.";
            destroy_channel_map(channel_map);
            return nullptr;
        }
        int timeout_ms = setting_iter->value.GetInt();
        // Retry count.
        setting_iter = settings.FindMember("retry");
        if (setting_iter == settings.MemberEnd() || !setting_iter->value.IsInt()) {
            APP_LOG(ERROR) << "Invalid service settings for " << service_name
                << ", expecting type Int for property retry.";
            destroy_channel_map(channel_map);
            return nullptr;
        }
        int retry = setting_iter->value.GetInt();
        // Headers for a http request.
        std::vector<std::pair<std::string, std::string>> headers;
        setting_iter = settings.FindMember("headers");
        if (setting_iter != settings.MemberEnd() && setting_iter->value.IsObject()) {
            const rapidjson::Value& obj_headers = setting_iter->value;
            rapidjson::Value::ConstMemberIterator header_iter;
            for (header_iter = obj_headers.MemberBegin();
                    header_iter != obj_headers.MemberEnd(); ++header_iter) {
                std::string header_key = header_iter->name.GetString();
                if (!header_iter->value.IsString()) {
                    APP_LOG(ERROR) << "Invalid header value for " << header_key
                        << ", expecting type String for header value.";
                    destroy_channel_map(channel_map);
                    return nullptr;
                }
                std::string header_value = header_iter->value.GetString();
                headers.push_back(std::make_pair(header_key, header_value));
            }
        }

        BRPC_NAMESPACE::Channel* rpc_channel = nullptr;
        if (protocol == "http") {
            if (client.empty() || client == "brpc") {
                rpc_channel = new BRPC_NAMESPACE::Channel();
                BRPC_NAMESPACE::ChannelOptions options;
                options.protocol = BRPC_NAMESPACE::PROTOCOL_HTTP;
                options.timeout_ms = timeout_ms;
                options.max_retry = retry;
                int ret = rpc_channel->Init(naming_service_url.c_str(), load_balancer_name.c_str(), &options);
                if (ret != 0) {
                    APP_LOG(ERROR) << "Failed to init channel.";
                    delete rpc_channel;
                    destroy_channel_map(channel_map);
                    return nullptr;
                }
            } else if (client == "curl") {
                // curl does not need to init rpc channel
            } else {
                APP_LOG(ERROR) << "Unsupported client value [" << client << "].";
                destroy_channel_map(channel_map);
                return nullptr;
            }
        } else {
            APP_LOG(ERROR) << "Unsupported protocol [" << protocol
                << "] for service [" << service_name << "], skipped...";
            destroy_channel_map(channel_map);
            return nullptr;
        }

        RemoteServiceChannel service_channel {
            .name = service_name,
            .protocol = protocol,
            .channel = rpc_channel,
            .timeout_ms = timeout_ms,
            .max_retry = retry,
            .headers = headers
        };
        channel_map->insert({service_name, service_channel});
        APP_LOG(TRACE) << "Loaded service " << service_name;
    }

    return channel_map;
}

int RemoteServiceManager::call_http_by_BRPC_NAMESPACE(BRPC_NAMESPACE::Channel* channel,
                                            const std::string& url,
                                            const HttpMethod method,
                                            const std::vector<std::pair<std::string, std::string>>& headers,
                                            const std::string& payload,
                                            std::string& result,
                                            std::string& remote_side,
                                            int& latency) const {
    BRPC_NAMESPACE::Controller cntl;
    cntl.http_request().uri() = url.c_str();
    if (method == HTTP_METHOD_POST) {
        cntl.http_request().set_method(BRPC_NAMESPACE::HTTP_METHOD_POST);
        cntl.request_attachment().append(payload);
    }
    for (auto const& header: headers) {
        if (header.first == "Content-Type" || header.first == "content-type") {
            cntl.http_request().set_content_type(header.second);
            continue;
        }
        cntl.http_request().SetHeader(header.first, header.second);
    }

    channel->CallMethod(NULL, &cntl, NULL, NULL, NULL);
    if (cntl.Failed()) {
        APP_LOG(WARNING) << "Call failed, error: " << cntl.ErrorText();
        remote_side = BUTIL_NAMESPACE::endpoint2str(cntl.remote_side()).c_str();
        latency = cntl.latency_us() / 1000;
        return -1;
    }
    result = cntl.response_attachment().to_string();

    remote_side = BUTIL_NAMESPACE::endpoint2str(cntl.remote_side()).c_str();
    latency = cntl.latency_us() / 1000;

    return 0;
}

static size_t curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    BUTIL_NAMESPACE::IOBuf* buffer = static_cast<BUTIL_NAMESPACE::IOBuf*>(userp);
    size_t realsize = size * nmemb;
    buffer->append(contents, realsize);
    return realsize;
}

int RemoteServiceManager::call_http_by_curl(const std::string& url,
                                            const HttpMethod method,
                                            const std::vector<std::pair<std::string, std::string>>& headers,
                                            const std::string& payload,
                                            const int timeout_ms,
                                            const int max_retry,
                                            std::string& result,
                                            std::string& remote_side,
                                            int& latency) const {
    // curl does not support retry now
    (void)max_retry;
    CURL *curl;
    CURLcode res;
    struct curl_slist *curl_headers = nullptr;
    curl = curl_easy_init();
    if (!curl) {
        APP_LOG(ERROR) << "Failed to init curl";
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    BUTIL_NAMESPACE::IOBuf response_buffer;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(&response_buffer));

    if (method == HTTP_METHOD_POST) {
         curl_easy_setopt(curl, CURLOPT_POST, 1L);
         curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
         curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload.length());
    }

    for (auto const& header: headers) {
        std::string header_value = header.first;
        header_value += ": ";
        header_value += header.second;
        curl_headers = curl_slist_append(curl_headers, header_value.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    res = curl_easy_perform(curl);
    curl_slist_free_all(curl_headers);
    if(res != CURLE_OK) {
        APP_LOG(ERROR) << "curl failed, error: " << curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return -1;
    }

    double total_time;
    res = curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);
    if (CURLE_OK == res) {
        latency = total_time * 1000;
    }
    char *ip = nullptr;
    res = curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
    if (CURLE_OK == res && ip != nullptr) {
        remote_side = ip;
    }

    curl_easy_cleanup(curl);
    result = response_buffer.to_string();

    return 0;
}

} // namespace dmkit
