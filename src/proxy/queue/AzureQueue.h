

#ifndef __AZUREQUEUE_H__
#define __AZUREQUEUE_H__

#include <map>
#include <string>
#include <vector>
#include "interfaces/IEventListener.h"
#include <was/queue.h>

class AcceptEvent;
class CloseEvent;
class LoginEvent;
class Miner;
class SubmitEvent;


namespace xmrig {
    class Controller;
}

class AzureQueue : public IEventListener
{
public:
    AzureQueue(xmrig::Controller *controller);
    ~AzureQueue();

protected:
    void onEvent(IEvent *event) override;
    void onRejectedEvent(IEvent *event) override;

private:
    void accept(const AcceptEvent *event);
    void reject(const SubmitEvent *event);
    
    bool m_enabled;
    xmrig::Controller *m_controller;
    azure::storage::cloud_queue_client m_queue_client;
    azure::storage::cloud_queue m_queue;
};


#endif /* __AZUREQUEUE_H__ */
