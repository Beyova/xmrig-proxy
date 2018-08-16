
#include "proxy/queue/AzureQueue.h"
#include "core/Config.h"
#include "core/Controller.h"
#include "proxy/events/AcceptEvent.h"
#include "proxy/events/CloseEvent.h"
#include "proxy/events/LoginEvent.h"
#include "proxy/events/SubmitEvent.h"
#include "common/log/Log.h"
#include "common/net/SubmitResult.h"
#include <was/storage_account.h>

AzureQueue::AzureQueue(xmrig::Controller *controller) :
    m_enabled(controller->config()->isWorkers()),
    m_controller(controller)
{
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


AzureQueue::~AzureQueue()
{
}

void AzureQueue::onEvent(IEvent *event)
{
    if (!m_enabled) {
        return;
    }

    switch (event->type())
    {
    case IEvent::AcceptType:
        accept(static_cast<AcceptEvent*>(event));
        break;

    default:
        break;
    }
}


void AzureQueue::onRejectedEvent(IEvent *event)
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

void AzureQueue::accept(const AcceptEvent *event)
{
    if (!m_enabled) {
        return;
    }

    const SubmitResult &result = event->result;
    std::ostringstream stream;
    stream << "accept diif=" << result.diff;
    try 
    {
        azure::storage::cloud_queue_message message(_XPLATSTR(stream.str()));
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

void AzureQueue::reject(const SubmitEvent *event)
{
    LOG_INFO("reject..");
}
