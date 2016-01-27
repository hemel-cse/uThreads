/*******************************************************************************
 *     Copyright © 2015, 2016 Saman Barghi
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/

#include "kThread.h"
#include "BlockingSync.h"
#include <unistd.h>

std::atomic_uint kThread::totalNumberofKTs(0);

__thread kThread* kThread::currentKT                    = nullptr;
__thread IntrusiveList<uThread>* kThread::ktReadyQueue  = nullptr;
//__thread uThread* kThread::currentUT = nullptr;


kThread::kThread(bool initial) : cv_flag(true), threadSelf(){       //This is only for initial kThread
	localCluster = &Cluster::defaultCluster;
	initialize(true);
	uThread::initUT = uThread::createMainUT(Cluster::defaultCluster);
	currentUT = uThread::initUT;                                                //Current running uThread is the initial one

	initialSynchronization();
}
kThread::kThread(Cluster& cluster, std::function<void(ptr_t)> func, ptr_t args) : localCluster(&cluster), threadSelf(&kThread::runWithFunc, this, func, args){
	initialSynchronization();
}

kThread::kThread(Cluster& cluster) : localCluster(&cluster), cv_flag(false), threadSelf(&kThread::run, this){
	initialSynchronization();
}

kThread::kThread() : localCluster(&Cluster::defaultCluster), threadSelf(&kThread::run, this){
	initialSynchronization();
}

kThread::~kThread() {
	totalNumberofKTs--;
	localCluster->numberOfkThreads--;

	//free thread local members
	//delete kThread::ktReadyQueue;
	//mainUT->destory(true);
}

void kThread::initialSynchronization(){
	totalNumberofKTs++;
	localCluster->numberOfkThreads++;											//Increas the number of kThreads in the cluster
}

void kThread::run() {
	initialize(false);
	defaultRun(this);
}
void kThread::runWithFunc(std::function<void(ptr_t)> func, ptr_t args) {
	initialize(false);
	func(args);
}

void kThread::switchContext(uThread* ut,void* args) {
    assert(ut != nullptr);
    assert(ut->stackPointer != 0);
	stackSwitch(ut, args, &kThread::currentKT->currentUT->stackPointer, ut->stackPointer, postSwitchFunc);
}

void kThread::switchContext(void* args){
	uThread* ut = nullptr;
	/*	First check the internal queue */
    IntrusiveList<uThread>* ktrq = ktReadyQueue;
	if(!ktrq->empty()){										//If not empty, grab a uThread and run it
		ut = ktrq->front();
		ktrq->pop_front();
	}else{													//If empty try to fill

	    localCluster->tryGetWorks(*ktrq);				//Try to fill the local queue
		if(!ktrq->empty()){								//If there is more work start using it
			ut = ktrq->front();
			ktrq->pop_front();
		}else{												//If no work is available, Switch to defaultUt
			if(kThread::currentKT->currentUT->state == YIELD)	return;				//if the running uThread yielded, continue running it
			ut = mainUT;
		}
	}
	assert(ut != nullptr);
	switchContext(ut,args);
}

void kThread::initialize(bool isDefaultKT) {
	kThread::currentKT		=	this;											//Set the thread_locl pointer to this thread, later we can find the executing thread by referring to this
	kThread::ktReadyQueue = new IntrusiveList<uThread>();

	if(slowpath(isDefaultKT)){
	    mainUT = uThread::create(defaultStackSize);
        mainUT->start(*localCluster, (ptr_t)kThread::defaultRun, this, nullptr, nullptr); //if defaultKT, then create a stack for mainUT cause pthread stack is assigned to initUT
    }
	else{
	    mainUT = uThread::createMainUT(*localCluster);			                    //Default function takes up the default pthread's stack pointer and run from there
	}

	uThread::totalNumberofUTs--; //Default uThreads are not counted as valid work
	mainUT->state =	READY;
	currentUT         =   mainUT;
}

void kThread::defaultRun(void* args) {
    kThread* thisKT = (kThread*) args;
    uThread* ut = nullptr;

    while (true) {
        //TODO: break this loop when total number of uThreads are less than 1, and end the kThread
        thisKT->localCluster->getWork(*thisKT->ktReadyQueue);
        assert(!ktReadyQueue->empty());                         //ktReadyQueue should not be empty at this point
        ut = thisKT->ktReadyQueue->front();
        thisKT->ktReadyQueue->pop_front();

        thisKT->switchContext(ut, nullptr);                     //Switch to the new uThread
    }
}

void kThread::postSwitchFunc(uThread* nextuThread, void* args=nullptr) {

    kThread* ck = kThread::currentKT;
	if(fastpath(ck->currentUT != kThread::currentKT->mainUT)){			//DefaultUThread do not need to be managed here
		switch (ck->currentUT->state) {
			case TERMINATED:
				ck->currentUT->destory(false);
				break;
			case YIELD:
				ck->currentUT->resume();;
				break;
			case MIGRATE:
				ck->currentUT->resume();
				break;
			case WAITING:
			{
			    assert(args != nullptr);
			    std::function<void()>* func = (std::function<void()>*)args;
			    (*func)();
				break;
			}
			default:
				break;
		}
	}
	ck->currentUT	= nextuThread;								//Change the current thread to the next
	nextuThread->state = RUNNING;
}

std::thread::native_handle_type kThread::getThreadNativeHandle() {
	return threadSelf.native_handle();
}

std::thread::id kThread::getThreadID() {
	return threadSelf.get_id();
}