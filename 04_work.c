#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
//#include "threadpool.h"
#include <sys/types.h>
#include <signal.h>
#include<sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

#define DEFAULT_TIME 10 //每10秒再adjust_thread 线程管理
#define MIN_WAIT_TASK_NUM 10 
#define DEFAULT_THREAD_VARY 10 //每次创建和销毁的线程个数

#define true 1
#define false 0

typedef struct{
    void *(*function)(void*);
    void *arg;
}threadpool_task_t;

int listenfd,ren;
typedef struct threadpool_t threadpool_t;
threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size);
int threadpool_add(threadpool_t *pool, void*(*function)(void *arg), void *arg);
int threadpool_destroy(threadpool_t *pool);
int threadpool_all_threadnum(threadpool_t *pool);
int threadpool_busy_threadnum(threadpool_t *pool);


//每个线程执行的工作线程
void *threadpool_thread(void *threadpool);
//管理线程
void *adjust_thread(void *threadpool);
//判断这个线程死否死了
int is_thread_alive(pthread_t tid);
//释放threadpool_task_t;
int threadpool_free(threadpool_t *pool);

threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size);

struct threadpool_t{
    pthread_mutex_t lock;//用于锁住本结构体
    pthread_mutex_t thread_counter;//记录忙状态线程个数得锁
    pthread_cond_t queue_not_full;//当任务队列满时，添加任务线程阻塞，等待次条件变量
    pthread_cond_t queue_not_empty;//任务队列里不为空时， 通知等待任务的线程

    pthread_t *threads;//存放线程池中的每个线程tid
    pthread_t adjust_tid;//存管理线程tid
    threadpool_task_t *task_queue;//任务队列

    int min_thr_num;//线程池最小线程数
    int max_thr_num;//线程池最大线程数量
    int live_thr_num;//当前存活线程个数
    int busy_thr_num;//忙状态线程个数
    int wait_exit_thr_num;//要销毁的线程个数

    int queue_front;//队头下标
    int queue_rear;//队尾下表
    int queue_size;//队中实际任务数
    int queue_max_size;//队列最大可容纳任务数

    int shutdown;;//标志位，线程池使用状态 
};

threadpool_t *threadpool_create(int min_thr_num,int max_thr_num,int queue_max_size){
    int i;
    threadpool_t *pool = NULL;
    do{
        if((pool=(threadpool_t *)malloc(sizeof(threadpool_t)))==NULL){
            printf("分配内存出错! -1\n ");
            break;
        }
        pool->min_thr_num = min_thr_num;//最小线程个数
        pool->max_thr_num = max_thr_num;//最大线程个数
        pool->busy_thr_num =0;//忙状态的线程
        pool->live_thr_num = min_thr_num;//活着的线程数 初值 = 最小的线程数
        pool->queue_size = 0;//队列中实际任务
        pool->queue_max_size = queue_max_size;//队列最大
        pool->queue_front = 0;//头
        pool->queue_rear = 0;//尾
        pool->shutdown = false;//不关闭线程池

        //根据最大线程上线数，给工作线程数组开辟空间 ，并清为0
        pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * max_thr_num);
        if(pool->threads == NULL){
            printf("分配内存失败 -2 \n");
            break;
        }
        memset(pool->threads,0,sizeof(pthread_t)*max_thr_num);

        //队列开辟空间
        pool->task_queue = (threadpool_task_t *)malloc(sizeof(threadpool_task_t)*queue_max_size);
        if(pool->task_queue == NULL){
            printf("队列开辟空间失败\n");
            break;
        }
        //初始化互斥锁 ，条件变量
        if(pthread_mutex_init(&(pool->lock),NULL)!=0 || 
           pthread_mutex_init(&(pool->thread_counter),NULL)!=0 || 
           pthread_cond_init(&(pool->queue_not_full),NULL) !=0 || 
           pthread_cond_init(&(pool->queue_not_empty),NULL) !=0){
            printf("初始化 锁失败\n");
            break;
        }
        // if(pthread_cond_init(&(pool->queue_not_empty),NULL) !=0 || pthread_cond_init(&(pool->queue_not_full),NULL)!=0){
        // printf("初始化 条件变量失败 \n");
        //break;
        // }
        //启动 min_thr_num个 工作线程
        for(i =0;i<min_thr_num;i++){
            pthread_create(&(pool->threads[i]),NULL,threadpool_thread,(void *)pool);//创建
            printf("--->成功创建线程:0x%x\n",(unsigned int)pool->threads[i]);
        }
        pthread_create(&(pool->adjust_tid),NULL,adjust_thread,(void*)pool);
        return pool;
    }while(0);
    threadpool_free(pool);//前面代码调用失败，释放poll存储空间
    return NULL;
}
//向线程池中 添加一个任务
int threadpool_add(threadpool_t *pool,void *(function)(void *arg),void *arg){

    pthread_mutex_lock(&(pool->lock));//锁住
    //== 为真 队列已满，调用wait阻塞
    while((pool->queue_size == pool->queue_max_size) && (!pool->shutdown)){
        printf("会阻塞\n");
        pthread_cond_wait(&(pool->queue_not_full),&(pool->lock));
    }

    if(pool->shutdown){
        pthread_mutex_unlock(&(pool->lock));
    }

    //清空工作线程 调用的回掉函数的 参数arg
    if(pool->task_queue[pool->queue_rear].arg !=NULL){
        free(pool->task_queue[pool->queue_rear].arg);
        pool->task_queue[pool->queue_rear].arg = NULL;
    }

    //    printf("目前 队头: %d  队尾：%d 队列实际任务数 %d\n :",pool->queue_front,pool->queue_rear,pool->queue_size);
    //添加到任务到队列里面
    pool->task_queue[pool->queue_rear].function = function;
    pool->task_queue[pool->queue_rear].arg = arg;
    pool->queue_rear = (pool->queue_rear +1) % pool->queue_max_size;//队尾指针下移
    pool->queue_size++;

    //添加完任务后 队列不为空 唤醒线程池中等待处理任务的线程
    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->lock));
    return 0;
}

//线程池中各个工作线程
void *threadpool_thread(void *threadpool){
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;
    printf("进入工作线程 \n");
    while(true){
        //刚创建出线程，等待任务队列里面有任务，否则阻塞等待任务队列里有任务后再唤醒接受任务
        pthread_mutex_lock(&(pool->lock));//抢锁，如果这个线程抢到了，那么其他线程会在外面

        //queue_size ==0 说明 没有任务，调用wait阻塞再条件变量上，若有任务，会跳过while
        while((pool->queue_size)== 0 && (!pool->shutdown)){ //任务等于0 且 pool没关机
            printf("--->>[线程: 0X%x 开始等待 ]\n ",(unsigned int)pthread_self());
            pthread_cond_wait(&(pool->queue_not_empty),&(pool->lock));//锁住pool-queue_not_empty 被唤醒后再去抢pool->lock

            //清除指定书目的空闲线程，如果要结束的线程个数大于0 结束线程
            if(pool->wait_exit_thr_num > 0 ){
                pool->wait_exit_thr_num--;

                //如果线程池里面的线程个数大于最小值时 可以结束当前线程
                if(pool->live_thr_num > pool->min_thr_num){
                    printf("-1>线程:0X%x 退出\n",(unsigned int)pthread_self());
                    pool->live_thr_num--;//当前线程--
                    pthread_mutex_unlock(&(pool->lock));//解锁
                    pthread_exit(NULL);//线程退出
                }

            }
        }
        //如果指定了ture，要关闭线程池里面的每个线程，自行退出
        if(pool->shutdown){
            pthread_mutex_unlock(&pool->lock);//解锁
            printf("-------->线程:0X%x 执行【退出】\n",(unsigned int)pthread_self());
            pthread_exit(NULL);//线程自行退出
        }
        //从任务队列里获取任务，是一个出队操作
        task.function = pool->task_queue[pool->queue_front].function;
        task.arg = pool->task_queue[pool->queue_front].arg;

        pool->queue_front = (pool->queue_front+1)% pool->queue_max_size;//出队，模拟环形队列
        pool->queue_size--;//大小--

        //通知可以有新的任务添加进来
        pthread_cond_broadcast(&(pool->queue_not_full));//唤醒full所有线程
        //任务取出后，立刻 把线程池锁 释放
        pthread_mutex_unlock(&(pool->lock));

        //执行任务
        printf("-->线程:0X%x 执行【任务】\n",(unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));//锁住 线程忙状态的锁
        pool->busy_thr_num++;//忙线程状态+1
        pthread_mutex_unlock(&(pool->thread_counter));//解锁 忙状态的锁
       
        (*(task.function))(task.arg);//执行任务的回掉函数


        printf("-->线程:0X%x 执行完毕【任务】\n",(unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));//锁忙状态           
        pool->busy_thr_num--;//处理掉一个任务，忙状态线程数量-1
        pthread_mutex_unlock(&(pool->thread_counter));//解锁忙状态的锁
    }
    pthread_exit(NULL);//执行完毕退出
}

//管理线程
void *adjust_thread(void *threadpool){
    int i;
    threadpool_t *pool = (threadpool_t *)threadpool;

    while(!pool->shutdown){//不等关机
        printf("【---进入管理线程---】\n");
        sleep(DEFAULT_TIME);//定时队线程池管理
        pthread_mutex_lock(&(pool->lock)); //锁住本管理线程
        int queue_size = pool->queue_size;//关注任务数
        int live_thr_num = pool->live_thr_num;//存货线程数
        pthread_mutex_unlock(&(pool->lock));//解锁


        pthread_mutex_lock(&(pool->thread_counter));//锁住忙着的线程
        int busy_thr_num = pool->busy_thr_num ;//忙着的线程数
        pthread_mutex_unlock(&(pool->thread_counter));//解锁忙着的线程


        //创建新线程算法：任务数量大于最小线程池个数 ，且存活的线程数少于最大线程个数时
        if(queue_size >= MIN_WAIT_TASK_NUM && live_thr_num < pool->max_thr_num){
            pthread_mutex_lock(&(pool->lock));//锁住
            int add = 0;

            //每次增加 DEFAULT_THREAD_VARY个线程
            for(i = 0 ; i <pool->max_thr_num && add<DEFAULT_THREAD_VARY && pool->live_thr_num < pool->max_thr_num;i++){
                if(pool->threads[i]==0 || !is_thread_alive(pool->threads[i])){
                    printf(" ---- 增加处理任务线程 \n");
                    pthread_create(&(pool->threads[i]),NULL,threadpool_thread,(void *)pool);
                    add++;
                    pool->live_thr_num++;
                }
            }
            pthread_mutex_unlock(&(pool->lock));
        }


        //销毁多余的空闲线程，算法：忙线程X2 小于存活的线程数 且 存货的线程数 大于 最小线程数
        if((busy_thr_num *2 )<live_thr_num && live_thr_num > pool->min_thr_num){
            //一次销毁 DEFAULT_THREAD_VARY 个线程，随机10个即可
            printf("进入销毁线程 \n");

            pthread_mutex_lock(&(pool->lock));
            pool->wait_exit_thr_num = DEFAULT_THREAD_VARY ;//要销毁的线程设置为10 即可
            pthread_mutex_unlock(&(pool->lock));


            //写这个代码的人 真的机智。
            for(i = 0 ; i < DEFAULT_THREAD_VARY ;i++){

                pthread_cond_signal(&(pool->queue_not_empty));
            }
        } 

        if(pool->busy_thr_num<=0){
            printf("------目前任务已经执行完毕，如果想结束按CTRL+C,或 等待对端的用户进行下一步操作 ------\n");
        }
    }
    return NULL;
}

int threadpool_destroy(threadpool_t *pool){

    int i;
    if(pool==NULL){
        return -1;
    }
    pool->shutdown = true;
    printf("-----------如果销毁的时候，服务端还有存在客户端的连接，需要按CTRL+C强制销毁，或等待客户端退出\n");
    close(listenfd);
    //先销毁管理线程
    pthread_join(pool->adjust_tid,NULL);
    for(i = 0 ; i < pool->live_thr_num;i++){
        pthread_cond_broadcast(&(pool->queue_not_empty));
    }

    for(i =0;i<pool->live_thr_num ;i++){
        pthread_join(pool->threads[i],NULL);
    }
    
    threadpool_free(pool);
    return 0;
}

int is_thread_alive(pthread_t tid){
    int kill_rc = pthread_kill(tid,0);//发0信号，测试线程是否存活
    if(kill_rc = ESRCH){ //线程不存活
        return false;
    }
    return true;
}

int threadpool_free(threadpool_t *pool)
{
    if(pool==NULL){
        return -1;
    }
    if(pool->task_queue){
        free(pool->task_queue);//释放任务队列
    }
    if(pool->threads){
        free(pool->threads);
        pthread_mutex_lock(&(pool->lock));//尝试加锁
        pthread_mutex_destroy(&(pool->lock));//销毁锁
        pthread_mutex_lock(&(pool->thread_counter));
        pthread_mutex_destroy(&(pool->thread_counter));
        pthread_cond_destroy(&(pool->queue_not_empty));//销毁变量
        pthread_cond_destroy(&(pool->queue_not_full));//销毁
    }
    free(pool);
    pool=NULL;
    return 0;
}
//模拟测试业务
void *process(void *arg){
    int cfd = *(int *)arg;
    printf("-->>线程:0x%x 开始处理【工作】%d\n ",(unsigned int)pthread_self(),cfd);
    char buf[256]={0};
    while(1){
        int ret = read(cfd,buf,sizeof(buf));
        if(ret <0){
            printf("客户端 %d 出现了异常，进行退出\n",cfd);
            close(cfd);
            break;
            }else if(ret ==0 ){
            printf("客户端退出了 %d\n",cfd);
            close(cfd);
            break;
            }else{
                int pd =strcmp(buf,"HELLO\n");
                int pd1=strcmp(buf,"WORLD\n");
                int pd2=strcmp(buf,"HELLO");
                int pd3=strcmp(buf,"WORLD");
            //因为时间关系，这边的代码写的非常冗余。
            if(!pd){
                memset(buf,0,sizeof(buf));
                strcpy(buf,"world\n");
                write(cfd,buf,strlen(buf));
            }else if(!pd1){
                memset(buf,0,sizeof(buf));
                strcpy(buf,"hello\n");
                write(cfd,buf,strlen(buf));
            }else if(!pd2){
                memset(buf,0,sizeof(buf));
                strcpy(buf,"world\n");
                write(cfd,buf,strlen(buf));
            }else if(!pd3){
                memset(buf,0,sizeof(buf));
                strcpy(buf,"hello\n");
                write(cfd,buf,strlen(buf));
            }else{
                for(int i=0;i<ret;i++){
                    buf[i] = toupper(buf[i]);
                }
                write(cfd,buf,ret);//写回给对端
                }
            }
    }

    printf("------------->>线程:0x%x 处理完毕【工作】%d\n ",(unsigned int)pthread_self(),cfd);
    ren--;
    return NULL;
}

threadpool_t *thp;
void siwang(int num){
    printf("正在释放资源中，即将结束\n");
    threadpool_destroy(thp);
    printf("--结束\n");
    exit(1);
}

int main(void){
    thp = threadpool_create(3,1000,1000);//创建线程池，线程池最小位3，最大位1000，最大队列为1000
    printf("线程池创建完毕\n");
    //int num[5000],i;

    listenfd = socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in serv,client;
    bzero(&serv,sizeof(serv));
    serv.sin_port = htons(8888);
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listenfd,(struct sockaddr*)&serv,sizeof(serv));
    listen(listenfd,128);

    char strIP[16];
    
    signal(SIGINT,siwang);
    while(1){
        if(ren >= 999){
            printf("服务端已经到达最大负荷，人数已满 拒绝其他玩家加入 -1\n");
        }
        socklen_t len = sizeof(client);
        int connfd = accept(listenfd,(struct sockaddr *)&client,&len);
        printf("新的连接来了 %s at %d\n",inet_ntop(AF_INET,&client.sin_addr.s_addr,strIP,sizeof(strIP)),ntohs(client.sin_port));
        threadpool_add(thp,process,(void *)&connfd);//添加任务 
        ren++;
     }
    // sleep(10);
    //printf("任务添加完完毕。\n");
    //    struct itimerval it = {{3,0},{0,0}};

    return 0;
}
