/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2016-2018 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include <inttypes.h>


#include "common/log/Log.h"
#include "common/net/SubmitResult.h"
#include "core/Config.h"
#include "core/Controller.h"
#include "proxy/events/AcceptEvent.h"
#include "proxy/events/CloseEvent.h"
#include "proxy/events/LoginEvent.h"
#include "proxy/events/SubmitEvent.h"
#include "proxy/LoginRequest.h"
#include "proxy/Miner.h"
#include "proxy/workers/Workers.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <was/storage_account.h>

Workers::Workers(xmrig::Controller *controller) :
    m_enabled(controller->config()->isWorkers()),
    m_controller(controller)
{
    m_algo_short_name = controller->config()->pools().front().algorithm().shortName();
    try
    {
        std::string connectionString(controller->config()->queueConnectionString());
        azure::storage::cloud_storage_account storage_account = azure::storage::cloud_storage_account::parse(connectionString);
        m_queue_client = storage_account.create_cloud_queue_client();
        m_queue = m_queue_client.get_queue_reference(_XPLATSTR("submitaudit"));
    }
    catch (const azure::storage::storage_exception& e)
    {
        ucout << _XPLATSTR("Error: ") << e.what() << std::endl;

        azure::storage::request_result result = e.result();
        azure::storage::storage_extended_error extended_error = result.extended_error();
        if (!extended_error.message().empty())
        {
            ucout << extended_error.message() << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        ucout << _XPLATSTR("Error: ") << e.what() << std::endl;
    }
}


Workers::~Workers()
{
}


void Workers::printWorkers()
{
    if (!m_enabled) {
        LOG_ERR("Per worker statistics disabled");

        return;
    }

    char workerName[24];
    size_t size = 0;

    Log::i()->text(m_controller->config()->isColors() ? "\x1B[01;37m%-23s | %-15s | %-5s | %-8s | %-3s | %11s | %11s |" : "%-23s | %-15s | %-5s | %-8s | %-3s | %11s | %11s |",
                   "WORKER NAME", "LAST IP", "COUNT", "ACCEPTED", "REJ", "10 MINUTES", "24 HOURS");

    for (const Worker &worker : m_workers) {
        const char *name = worker.name();
        size = strlen(name);

        if (size > sizeof(workerName) - 1) {
            memcpy(workerName, name, 6);
            memcpy(workerName + 6, "...", 3);
            memcpy(workerName + 9, name + size - sizeof(workerName) + 10, sizeof(workerName) - 10);
            workerName[sizeof(workerName) - 1] = '\0';
        }
        else {
            strncpy(workerName, name, sizeof(workerName) - 1);
        }

        Log::i()->text("%-23s | %-15s | %5" PRIu64 " | %8" PRIu64 " | %3" PRIu64 " | %6.2f kH/s | %6.2f kH/s |",
                       workerName, worker.ip(), worker.connections(), worker.accepted(), worker.rejected(), worker.hashrate(600), worker.hashrate(86400));
    }
}


void Workers::tick(uint64_t ticks)
{
    if ((ticks % 4) != 0) {
        return;
    }

    for (Worker &worker : m_workers) {
        worker.tick(ticks);
    }
}


void Workers::onEvent(IEvent *event)
{
    if (!m_enabled) {
        return;
    }

    switch (event->type())
    {
    case IEvent::LoginType:
        login(static_cast<LoginEvent*>(event));
        break;

    case IEvent::CloseType:
        remove(static_cast<CloseEvent*>(event));
        break;

    case IEvent::AcceptType:
        accept(static_cast<AcceptEvent*>(event));
        break;

    default:
        break;
    }
}


void Workers::onRejectedEvent(IEvent *event)
{
    if (!m_enabled) {
        return;
    }

    switch (event->type())
    {
    case IEvent::SubmitType:
        reject(static_cast<SubmitEvent*>(event));
        break;

    case IEvent::AcceptType:
        accept(static_cast<AcceptEvent*>(event));
        break;

    default:
        break;
    }
}


bool Workers::indexByMiner(const Miner *miner, size_t *index) const
{
    if (!miner || miner->mapperId() == -1 || m_miners.count(miner->id()) == 0) {
        return false;
    }

    const size_t i = m_miners.at(miner->id());
    if (i >= m_workers.size()) {
        return false;
    }

    *index = i;
    return true;
}


void Workers::accept(const AcceptEvent *event)
{
    size_t index = 0;
    if (!indexByMiner(event->miner(), &index)) {
        return;
    }

    Worker &worker = m_workers[index];
    if (!event->isRejected()) {
        worker.add(event->result);
        submitQueue(&worker, event->miner(), &event->result, "accpet");
    }
    else {
        worker.reject(false);
        submitQueue(&worker, event->miner(), nullptr, "reject");
    }
}


void Workers::login(const LoginEvent *event)
{
    const std::string name(event->request.rigId());

    if (m_map.count(name) == 0) {
        const size_t id = m_workers.size();
        m_workers.push_back(Worker(id, name, event->miner()->ip()));

        m_map[name] = id;
        m_miners[event->miner()->id()] = id;

        submitQueue(nullptr, event->miner(), nullptr, "login");
    }
    else {
        Worker &worker = m_workers[m_map.at(name)];

        worker.add(event->miner()->ip());
        m_miners[event->miner()->id()] = worker.id();

        submitQueue(&worker, event->miner(), nullptr, "login");
    }
}


void Workers::reject(const SubmitEvent *event)
{
    size_t index = 0;
    if (!indexByMiner(event->miner(), &index)) {
        return;
    }

    Worker &worker = m_workers[index];
    worker.reject(true);

    submitQueue(&worker, event->miner(), nullptr, "invalid");
}


void Workers::remove(const CloseEvent *event)
{
    size_t index = 0;
    if (!indexByMiner(event->miner(), &index)) {
        return;
    }
    Worker &worker = m_workers[index];
    worker.remove();

    submitQueue(&worker, event->miner(), nullptr, "logout");
}

void Workers::submitQueue(const Worker *worker, const Miner *miner, const SubmitResult *result, const char *eventName)
{
    rapidjson::Document doc;
    doc.SetObject();

    auto &allocator = doc.GetAllocator();

    const char *uid = "";
    const char *ip = "";
    uint32_t diff = 0;

    if (worker != nullptr) {
        uid = worker->name();
    }
    if (miner != nullptr) {
        ip = miner->ip();
    }
    if (result != nullptr) {
        diff = result->diff;
    }

    doc.AddMember("uid", rapidjson::StringRef(uid), allocator);
    doc.AddMember("ip", rapidjson::StringRef(ip), allocator);
    doc.AddMember("event", rapidjson::StringRef(eventName), allocator);
    doc.AddMember("algo", rapidjson::StringRef(m_algo_short_name), allocator);
    doc.AddMember("diff", diff, allocator);

    rapidjson::StringBuffer buffer;
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    std::string s(buffer.GetString());
    try
    {
        azure::storage::cloud_queue_message message(_XPLATSTR(s));
        m_queue.add_message_async(message);
    }
    catch (const azure::storage::storage_exception& e)
    {
        ucout << _XPLATSTR("Error: ") << e.what() << std::endl;

        azure::storage::request_result result = e.result();
        azure::storage::storage_extended_error extended_error = result.extended_error();
        if (!extended_error.message().empty())
        {
            ucout << extended_error.message() << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        ucout << _XPLATSTR("Error: ") << e.what() << std::endl;
    }
}
