#ifndef _DYNAMIC_PROCESS_POOL_H
#define _DYNAMIC_PROCESS_POOL_H

#include <functional>

#include <queue>
#include <vector>

#include <future>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

template <typename _IN, typename _OUT>
class DynamicProcessPool{
public:
	typedef std::function<_OUT(_IN)>handler_t;
	typedef std::thread				worker_t;

	typedef struct{
		std::promise<_OUT> *result;
		_IN item;
	} workPair_t;

	DynamicProcessPool(){
	}

	/*
		DynamicProcessPool

		_initialWorkers : 처음에 가지고 시작할 worker의 수
		_maxWorker : 최대 가질 수 있는 worker의 수
		_lifeTime : 한 개의 worker가 일을 몇 번 수행할지 횟수
		_handler : workItem을 핸들링할 핸들러
	*/
	DynamicProcessPool(	int _initialWorkers,int _maxWorker,
						int _lifeTime, const handler_t &_handler) :
		handler( _handler ),
		quit( false ),
		maxWorker( _maxWorker ), lifeTime( _lifeTime ),
		nWorker( 0 ), nWaiting( 0 ), nWorking( 0 ) {

		for(int i=0;i<_initialWorkers;i++)
			addWorker( _lifeTime );
	}
	/*
		~DynamicProcessPool

		
	*/
	virtual ~DynamicProcessPool(){
		kill();
	}

	/*
		enqueue

		work queue에 workItem을 집어넣는다.

		workItem : 넣을 workItem
	*/
	std::future<_OUT> enqueue(_IN workItem){
		workPair_t workPair;
		
		workPair.result = new std::promise<_OUT>();
		workPair.item = workItem;
		
		// 비어있는 worker가 없고 maxWorker만큼 worker가 없으면
		// 새 worker를 생성하고 일을 할당.
		if( maxWorker < nWorker.load() && nWaiting.load() == 0 ){
			addWorkerWithWork( 10, workPair );
		}
		else{
			std::unique_lock<std::mutex> guard( queueMutex );
				qWork.push( workPair );
			guard.unlock();

			// signal을 기다리는 worker가 있을 때만 notify
			if( nWaiting.load() > 0 )
				signal.notify_one();
		}
		
		return workPair.result->get_future();
	}

	/*
		queryPoolStatus

		풀의 상태를 얻어온다.

		waiting : waiting중인 worker의 수를 받아올 포인터
		working : working중인 worker의 수를 받아올 포인터
	*/
	void queryPoolStatus(int *waiting,int *working){
		if( waiting != nullptr )
			*waiting = nWaiting.load();
		if( working != nullptr )
			*working = nWorking.load();
	}

	/*
		kill

		모든 worker를 죽인다.
	*/
	void kill(){
		postQuitWorkers();

		for( auto &worker : workers )
			worker.detach();
		workers.clear();

		// spin wait
		//   joinable, join 사이에 컨텍스트 스위칭을 막으려고 락을 쓰는 것 대신
		//   spin wait를 사용한다.
		int spincount = 10000;
		while( nWorker.load() > 0 ){
			if( spincount )
				spincount --;
			else{
				std::this_thread::sleep_for(
					std::chrono::milliseconds(1) );
			}
		}
	}

protected:
	void doWork(workPair_t &workPair){
		workPair.result->set_value(
			handler( workPair.item ) );
		delete workPair.result;
	}
	
	/*
		workthread

		worker 쓰레드
	*/
	void workthread(int lifeCount){
		nWorker.fetch_add( 1 );

		while( !quit && lifeCount > 0 ){
			workPair_t workPair;

			{	
				std::unique_lock<std::mutex> guard( queueMutex );
				
				// double check
				if( qWork.empty() ){
					nWaiting.fetch_add(1);
						signal.wait( guard );
					nWaiting.fetch_sub(1);	

					if( qWork.empty() )
						continue;
				}

				workPair = qWork.front();
				qWork.pop();
			}

			nWorking.fetch_add(1);
				doWork( workPair );
			nWorking.fetch_sub(1);

			lifeCount --;
		}

		nWorker.fetch_sub( 1 );
	}

	/*
		addWorker

		새 worker를 추가한다.

		lifeCount : 라이프카운트
	*/
	void addWorker(int lifeCount){
		auto boundMethod =
			std::bind( &DynamicProcessPool::workthread, this, std::placeholders::_1 );

		workers.push_back(
			std::thread( boundMethod, lifeCount ));
	}

	/*
		addWorkerWithWork

		새 worker를 추가하고 workItem을 넣어준다.

		lifeCount : 라이프카운트
		workItem : 생성과 후 바로 처리할 workItem
	*/
	void addWorkerWithWork(int lifeCount, workPair_t _workPair){
		workPair_t workPair = _workPair;

		workers.push_back(
			std::thread( [=](){
				handler( workPair.item );

				workthread( lifeCount );
			}));
	}

	/*
		postQuitWorkers

		모든 worker에게 종료 요청을 보낸다.
	*/
	void postQuitWorkers(){
		quit = true;

		signal.notify_all();
	}

protected:
	std::atomic<int> nWorker;	// 생성된 총 worker의 수 ( nWaiting + nWorking != nWorker )
	std::atomic<int> nWaiting;	// signal 을 기다리는 worker의 수
	std::atomic<int> nWorking;	// handler를 호출하여 일하고 있는 worker의 수

	std::vector<worker_t> workers;	// worker 인스턴스의 목록
	std::queue<workPair_t> qWork;	// work queue

	std::condition_variable signal;	// 시그날 객체
	std::mutex queueMutex;

	const handler_t handler;

	int lifeTime;
	int maxWorker;

	bool quit;	// postQuit 플래그
};

#endif //_DYNAMIC_PROCESS_POOL_H
