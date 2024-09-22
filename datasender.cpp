/* Краткое описание
Потоки находятся в постояных циклах.
Поток получения данных использует блокирующую функцию, после выполнения которой он обновляет очередь.
Это означает, что он не тратит процессорное время на бессмысленные действия (например проверки).

Поток отправки данных в даннной программе будет тратить впустую процессорное время, к сожалению.
Т.к. он постоянно выполняет проверку пустоты очереди.

Потокобезопасная очередь реализована с помощью класса очереди, содержащей мьютекс и std::quenue, доступные только через собственныее методы.
Методы, которые связаны с обновлением внутренних указателей очереди (push и pop) завлаедевают мьютексом при их выполнении.

Отправка данных по UDP выполняется с помощью датаграммного сокета.

Завершение программы выполняется при получении любого сигнала.
Обработчик завершения программы отправляет запрос на отмену потоков. Ждет обновления флага, очищает очередь (на всякий случай).
Потоки отменяются в безопасных точках. Это позволяет очистить очередь обработчиком завершения программы.
При отмене потока вызываются обработчики отмены, обновляющие флаги отмены потока.
*/

#include <iostream>
#include <thread>
#include <complex>
#include <mutex>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#define UDP_MESSAGE_SIZE (size_t)(64*1024) //Размер отправки по UDP, может быть определен стандартом. Получение данных от АЦП блоками того же размера.
#define IP_ADDR "FFFF204152189116" //IP-Адрес
#define PORT_NUM 50002 //Номер порта

class ThreadSaveQuenue //Класс, обеспечивающий потокобезопасность.
{
public:
    void push(std::complex<int16_t>* value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queque.push(value);
    }
    void pop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queque.pop();
    }
    bool empty_check (void)
    {
        return m_queque.empty();
    }
    std::complex<int16_t>* get_front (void)
    {
        return m_queque.front();
    }
private:
    std::queue<std::complex<int16_t>*> m_queque;
    mutable std::mutex m_mutex;
};

struct thread_cancel_struct{ //Структура, передаваемая обраб. завершения программы. Обеспечивает корректное завершение потоков (с ожидением флагов)
    pthread_t* t1;
    pthread_t* t2;
    int* cancel_flag1;
    int* cancel_flag2;
};

void create_thread(pthread_t *t, void *(*startfunc)(void *), void* flag);
void* GetterThreadMain(void* arg); //Стартовая точка потока отправки
void* SenderTreadMain(void* arg); //Стартовая точка потока получения данных
void onexit_handeler(int status, void* arg); //Обработчик завершения процесса
void atcancel_sockclose(void *arg); //Обработчик отмены потока, закрывает сокет перед отменой
void atcancel_handler(void* arg); //Обработчик отмены потока, устанавливает флаг
std::complex<int16_t>* GetDmaBuff(size_t N) //Функия прямого доступа к памяти
{
    std::complex<int16_t>* temp_ptr = (std::complex<int16_t>*)malloc(sizeof(std::complex<int16_t>));
    return temp_ptr;
}

static ThreadSaveQuenue DataQuenue;

int main(int argc, char* argv[])
{

    pthread_t t1, t2;
    sigset_t main_set;
    int cancel_flag1 = 1;
    int cancel_flag2 = 1;
    struct thread_cancel_struct temp_struct = {&t1, &t2, &cancel_flag1, &cancel_flag2};

    //Добавления всех сигналов в сигнальную маску
    if(sigfillset(&main_set) == -1)
    {
        printf("Sigfillset error");
        exit(2);
    }
    if(sigprocmask(SIG_BLOCK, &main_set, NULL) == -1)
    {
        printf("Sigprocmask error");
        exit(3);
    }

    //Создание потоков
    create_thread(&t1, GetterThreadMain, &cancel_flag1);
    create_thread(&t2, SenderTreadMain, &cancel_flag2);

    //Установка обработчика выхода
    if(on_exit(onexit_handeler, &temp_struct) != 0)
    {
        printf("AtexitError");
        exit(7);
    }
    //Очищаем сигнальную маску
    if(sigprocmask(SIG_UNBLOCK, &main_set, NULL) == -1)
    {
        printf("Sigprocmask error");
        exit(5);
    }
    pause();

    return 0;
}

void onexit_handeler(int status, void* arg) //Обработчик завершения работы программы
{
    struct thread_cancel_struct* temp_ptr = (struct thread_cancel_struct*)arg;
    if(pthread_cancel(*temp_ptr->t1) != 0) //Отмена потоков
    {
        printf("PthCancelError");
        exit(6);
    }
    if(pthread_cancel(*temp_ptr->t2) != 0)
    {
        printf("PthCancelError");
        exit(6);
    }
    while((&temp_ptr->cancel_flag1) && (&temp_ptr->cancel_flag2)) //Ждем завершения (int pthread_cancel не ждет завершения потока)
    {
        continue;
    }
    while(!DataQuenue.empty_check())
    {
        DataQuenue.pop();
    }
}

void* GetterThreadMain(void* arg) //Стартовая точка потока получения данных
{
    pthread_cleanup_push(atcancel_handler, arg); //Изменит флаг при отмене

    std::complex<int16_t>* temp_ptr = NULL;
    while((temp_ptr = GetDmaBuff(UDP_MESSAGE_SIZE)) != NULL) //Бесконечный цикл получения данных (если не будет ошибки функции с возвратом NULL)
    {
        DataQuenue.push(temp_ptr);
        pthread_testcancel(); //Безопасная точка отмены
    }
    pthread_cleanup_pop(1);
    return NULL; //Никогда не будет возврата
}

void* SenderTreadMain(void* arg) //Стартовая точка потока отправки
{
    struct sockaddr_in6 svaddr;
    int socket_fd;
    if((socket_fd = socket(AF_INET6, SOCK_DGRAM, 0)) == -1)
    {
        exit(10);
    }
    svaddr.sin6_family = AF_INET6; //Семейство адресов
    svaddr.sin6_port = htons(PORT_NUM); //Порт для транспортного уровня, перевод в сетевой порядок байтов
    if (inet_pton(AF_INET6, IP_ADDR, &svaddr.sin6_addr) < 1) //Адрес для сетвого уровня
    {
        exit(11);
    }

    pthread_cleanup_push(atcancel_sockclose, (void*)&socket_fd); //Закроет сокет при отмене
    pthread_cleanup_push(atcancel_handler, arg); //Изменит флаг при отмене

    while(1) //Бесконечный цикл отправки данных
    {
        if(!DataQuenue.empty_check())
        {
            if((sendto(socket_fd, (void*)DataQuenue.get_front(), UDP_MESSAGE_SIZE, 0, (struct sockaddr*)&svaddr, sizeof(struct sockaddr_in6))) != UDP_MESSAGE_SIZE)
            {
                exit(11);
            }
            DataQuenue.pop();
        }
        pthread_testcancel(); //Безопасная точка отмены
    }
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    return NULL; //Никогда не будет возврата
}

void atcancel_sockclose(void *arg) //Закрытие сокета перед отменой потока
{
    if(close(*(int*)arg) != 0) //Не гарантирует закрытие сокета, если могут быть другие ссылающщиеся на него файловые дескрипторы
    {
        exit(12);
    }
}

void atcancel_handler(void* arg) //Установка флага перед отменой потока
{
    *(int*)arg = 0;
}

void create_thread(pthread_t *t, void *(*startfunc)(void *), void* arg) //Передача указателя на флаг отмены потока
{
    int s = pthread_create(t, NULL, startfunc, arg);
    if(s != 0)
    {
        exit(1);
    }
}
