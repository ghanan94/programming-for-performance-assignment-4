#define _XOPEN_SOURCE 500

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <sched.h>
#include <sys/time.h>
#include <math.h>
#include <stdarg.h>
#include <getopt.h>
#include <openssl/sha.h>
#include <string.h>

/* Do not modify any of the #define directives */
#define RANDOM_ASSIGNMENT 1
#define ROUND_ROBIN_ASSIGNMENT 2
#define HASH_BUFFER_LENGTH 32
#define OUTPUT_FILE_NAME "results.csv"


typedef struct job_t {
    int id;
    struct timeval arrival_time;
    struct timeval execution_time;
    struct timeval departure_time;
    unsigned char* data;
    unsigned char* output;
    int rounds;
    struct job_t* next;
    struct job_t* prev;
} job_t;

typedef struct {
    job_t* head;
    job_t* tail;
    int total_rounds;
    pthread_mutex_t* mutex;
} queue_t;

/* Some sensible defaults */
int num_queues = 4;
int policy = ROUND_ROBIN_ASSIGNMENT;
int num_jobs = 100000;
int balance_load = 0;
int max_rounds = 2000;
int lambda = 100;

unsigned int generator_seed;
int total_completed = 0;
int terminate = 0;
pthread_mutex_t completed_mutex = PTHREAD_MUTEX_INITIALIZER;
int id = 0;
FILE* csv;

void *load_balance( void* args );
void *generate( void* arg );
void execute( job_t* toExecute );
void write_to_file( job_t* toWrite );
void *fetch_and_execute( void* arg );
void enqueue( job_t* toEnqueue, unsigned int* generator_seed );

queue_t * queues;
pthread_t * threads;

//
// Not thread-safe implementation
//
static inline void _add_to_queue(queue_t* queue, job_t* job)
{
    queue->total_rounds += job->rounds;

    if (queue->head == NULL)
    {
        queue->head = job;
    }
    else
    {
        /*
        job_t* j = queue->head;
        while ( j->next != NULL ) {
            j = j->next;
        }
        j->next = job;
        */

        queue->tail->next = job;
        job->prev = queue->tail;
    }

    queue->tail = job;
}


//
// Thread safe (uses the not-thread safe implementation
// surrounded by the queue's mutex
//
static inline void add_to_queue(queue_t* queue, job_t* job)
{
    pthread_mutex_lock(queue->mutex);

    _add_to_queue(queue, job);

    pthread_mutex_unlock(queue->mutex);
}

/* error handling macro from Patrick Lam */
void abort_(const char * s, ...) {
  va_list args;
  va_start(args, s);
  vfprintf(stderr, s, args);
  fprintf(stderr, "\n");
  va_end(args);
  abort();
}

/* Generate random string code based off original by Ates Goral */
char* random_string(const int len, unsigned int * generator_seed) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    char* s = malloc( (len+1) * sizeof( char ));

    for (int i = 0; i < len; ++i) {
        s[i] = alphanum[rand_r(generator_seed) % (sizeof(alphanum) - 1)];
    }
    s[len] = 0;
    return s;
}

/* GNU libc documentation says this is how we subtract timevals correctly. */
int timeval_subtract (struct timeval *result, struct timeval *x, struct timeval *y) {
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_usec < y->tv_usec) {
    int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    y->tv_usec -= 1000000 * nsec;
    y->tv_sec += nsec;
  }
  if (x->tv_usec - y->tv_usec > 1000000) {
    int nsec = (x->tv_usec - y->tv_usec) / 1000000;
    y->tv_usec += 1000000 * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait. tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_usec = x->tv_usec - y->tv_usec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}


int main(int argc, char **argv) {
    int c;
    pthread_t generator;
    pthread_t loadbalancer;

    while ((c = getopt (argc, argv, "n:a:j:b:l:m:")) != -1) {
    switch (c) {
    case 'n':
      num_queues = strtoul(optarg, NULL, 10);
      if (num_queues <= 0) {
        printf("%s: option requires an argument > 0 -- 'n'\n", argv[0]);
        return -1;
      }
      break;
    case 'a':
      policy = strtoul(optarg, NULL, 10);
      if (policy <= 0 || policy > 2) {
        printf("%s: option requires an argument (1 for random; 2 for round robin) -- 'a'\n", argv[0]);
        return -1;
      }
      break;
    case 'j':
      num_jobs = strtoul(optarg, NULL, 10);
      if (num_jobs <= 0) {
         printf("%s: option requires an argument > 0 -- 'j'\n", argv[0]);
         return -1;
      }
      break;
    case 'b':
      balance_load = strtoul(optarg, NULL, 10);
      if (balance_load < 0 || balance_load > 1) {
         printf("%s: option requires an argument of either 0 or 1 -- 'l'\n", argv[0]);
         return -1;
      }
      break;
      case 'l':
      lambda = strtoul(optarg, NULL, 10);
      if (lambda <= 0) {
          printf("%s: option requires an argument > 0 -- 'l'\n", argv[0]);
          return -1;
      }
      break;
      case 'm':
      max_rounds = strtoul(optarg, NULL, 10);
      if (max_rounds <= 0) {
          printf("%s: option requires an argument > 0 -- 'm'\n", argv[0]);
          return -1;
      }
      break;
    default:
      return -1;
    }
  }

    printf("Starting up with %d queues, assignment policy %d, %d jobs, lambda %d, max rounds %d, and load balancing %d.\n",
        num_queues, policy, num_jobs, lambda, max_rounds, balance_load);

    csv = fopen(OUTPUT_FILE_NAME, "w+");


   /* Initialize the queues and pthreads and start them */

    queues = malloc( num_queues * sizeof( queue_t ) );
    threads = malloc( num_queues * sizeof( pthread_t ) );

    for ( int i = 0; i < num_queues; ++i ) {
        queues[i].head = NULL;
        queues[i].tail = NULL;
        queues[i].total_rounds = 0;
        queues[i].mutex = malloc( sizeof( pthread_mutex_t ) );
        pthread_mutex_init(queues[i].mutex, NULL);
    }

    for ( int j = 0; j < num_queues; ++j ) {
        pthread_create( &threads[j], NULL, fetch_and_execute, &queues[j]);
    }

    pthread_create( &generator, NULL, generate, NULL);

    if ( 1 == balance_load ) {
        pthread_create( &loadbalancer, NULL, load_balance, NULL);
    }

    pthread_join( generator, NULL );

    for ( int k = 0; k < num_queues; ++k ) {
        pthread_join( threads[k], NULL );
    }

    for ( int l = 0; l < num_queues; ++l ) {
        pthread_mutex_destroy(queues[l].mutex);
        free(queues[l].mutex);
    }

    if ( 1 == balance_load ) {
        pthread_join( loadbalancer, NULL );
    }

    free(queues);
    free( threads );
    fclose( csv );
    pthread_exit( NULL );

}



void *fetch_and_execute( void* arg ) {
    queue_t* my_q = (queue_t*) arg;


    while(0 == terminate) {
        pthread_mutex_lock( my_q->mutex );
        if (my_q->head == NULL) {
            pthread_mutex_unlock( my_q->mutex );
        } else {
            job_t* job = my_q->head;

            my_q->head = my_q->head->next;

            if (my_q->head == NULL) {
                my_q->tail = NULL;
            } else {
                my_q->head->prev = NULL;
            }

            my_q->total_rounds -= job->rounds;
            pthread_mutex_unlock( my_q->mutex );

            execute( job );
            write_to_file( job );
        }

    }
    pthread_exit( NULL );
}

void execute( job_t* job ) {

    struct timeval begin_execution;
    gettimeofday( &begin_execution, NULL );
    unsigned char* output_buffer = calloc( HASH_BUFFER_LENGTH , sizeof ( unsigned char ) );

    for( int i = 0; i < job->rounds; ++i ) {
        SHA256( job->data, HASH_BUFFER_LENGTH, output_buffer );
        memcpy( job->data, output_buffer, HASH_BUFFER_LENGTH );
    }
    job->output = output_buffer;

    gettimeofday( &job->departure_time, NULL );
    timeval_subtract (&job->execution_time, &(job->departure_time), &(begin_execution));
}

void *generate( void* arg ) {

    generator_seed = time(NULL);
    for (int i = 0; i < num_jobs; ++i) {
        job_t* new_job = calloc(1, sizeof( job_t ));
        gettimeofday(&new_job->arrival_time, NULL);
        new_job->id = id;
        ++id;

        new_job->rounds = ceil( (double)rand_r(&generator_seed)/(double)RAND_MAX * max_rounds );
        new_job->data = random_string( HASH_BUFFER_LENGTH, &generator_seed );

        enqueue( new_job, &generator_seed );

        /* Sleep time follows poisson process randomness... */
        double randval = (double)rand_r(&generator_seed)/(double)RAND_MAX;
        unsigned int sleep_time = floor (-log (randval ) * lambda);
        usleep( sleep_time );
    }

    /* Okay, that's all on that. */
    pthread_exit( NULL );
}

void write_to_file( job_t* job ) {

    struct timeval response_time;
    timeval_subtract (&response_time, &(job->departure_time), &(job->arrival_time));

    pthread_mutex_lock( &completed_mutex );

    /* Write to file should probably be serialized because jumbled output is bad.*/
    fprintf( csv, "%d,", job->id );
    fprintf( csv, "%ld.%06ld", job->arrival_time.tv_sec, job->arrival_time.tv_usec );
    fprintf( csv, "," );
    fprintf( csv, "%ld.%06ld", job->execution_time.tv_sec, job->execution_time.tv_usec);
    fprintf( csv, "," );
    fprintf( csv, "%ld.%06ld", job->departure_time.tv_sec, job->departure_time.tv_usec );
    fprintf( csv, "," );
    fprintf( csv, "%ld.%06ld", response_time.tv_sec, response_time.tv_usec );
    fprintf( csv, "\n" );


    /* Finished here / Greeting death / He comes to take away */
    total_completed++;
    if (total_completed == num_jobs) {
        terminate = 1;
    }
    pthread_mutex_unlock( &completed_mutex );

    free( job->data );
    free( job->output );
    free( job );
}

void enqueue( job_t* job, unsigned int * generator_seed ) {

    queue_t* selected;

    if (RANDOM_ASSIGNMENT == policy ) {
        selected = &queues[ rand_r(generator_seed) % num_queues ];
    } else if ( ROUND_ROBIN_ASSIGNMENT == policy ) {
        selected = &queues[ job->id % num_queues ];
    } else {
       abort_("[enqueue] Invalid assignment policy selected: %d\n", policy);
    }

    add_to_queue(selected, job);
}

void *load_balance( void* args ) {
    int avg;
    int i;
    int j;
    job_t* job;
    job_t* job_list;

    job_list = NULL;

    while (terminate == 0) {
        job_list = NULL;
        usleep(100000);

        //
        // Calculate avg number
        //
        avg = 0;
        for (i = 0; i < num_queues; ++i)
        {
            pthread_mutex_lock(queues[i].mutex);
            avg += queues[i].total_rounds;

        }
        avg /= num_queues;
        printf("Avg: %d\n", avg);

        if (avg > max_rounds)
        {

            //
            // max length of any queue should be avg
            //
            for (i = 0; i < num_queues; ++i)
            {
                if (queues[i].total_rounds > avg)
                {
                    //
                    // Find the point where we start to have
                    // too much work for this queue ("extra" jobs)
                    //

                    //
                    // Append "Extra" jobs to the queue temporarily
                    //
                    if (job_list != NULL)
                    {
                        job_list->prev = queues[i].tail;
                        queues[i].tail->next = job_list;
                    }

                    //
                    // Find the point where there stats to be an inblance in jobs
                    // (more than 1 job away from avg)
                    //
                    while ((queues[i].total_rounds - queues[i].tail->rounds) > avg) {
                        queues[i].total_rounds -= queues[i].tail->rounds;
                        queues[i].tail = queues[i].tail->prev;
                    }

                    //
                    // Detatch this queue from all jobs that are considered
                    // to make this queue unbalanced ("extra" jobs).
                    //
                    if (queues[i].tail->next != NULL)
                    {
                        job_list = queues[i].tail->next;
                        queues[i].tail->next = NULL;
                        job_list->prev = NULL;
                    }
                }
            }

            //
            // Add to any queues at have less than avg length (except last)
            //
            for (i = 0; i < num_queues - 1 && job_list; ++i) {
                if (queues[i].total_rounds < avg)
                {
                    //
                    // Append all "extra" jobs to this queue (to be trimmed)
                    //
                    _add_to_queue(&queues[i], job_list);

                    //
                    // Find the point where the total_rounds of this queue
                    // is equal to or just above average (make it approximately avg)
                    //
                    while (queues[i].total_rounds < avg && queues[i].tail->next) {
                        queues[i].tail = queues[i].tail->next;
                        queues[i].total_rounds += queues[i].tail->rounds;
                    }

                    //
                    // Update list of "extra" jobs to the one after
                    // the job that was just added to this queue
                    //
                    job_list = queues[i].tail->next;


                    //
                    // NOTE**************
                    //
                    // Commented this out because if job_list is not NULL,
                    // It's prev value will be written to in the following
                    // _add_to_queue call (if job_list is not NULL, _add_to_queue
                    // will be guaranteed to be called soon).
                    //
                    //if (job_list != NULL)
                    //{
                    //    job_list->prev = NULL;
                    //}
                    //

                    //
                    // Detach newly last job to this queue from rest
                    // of list of "extra" jobs
                    //
                    queues[i].tail->next = NULL;
                }
            }

            //
            // If there is still "extra" jobs left, put it in the final queue (everything else
            // is approximately avg so there shouldn't be many jobs left to throw off
            // the balance that much).
            //
            if (job_list != NULL) {
                //
                // Put rest in last queue.
                // either this queue has less than the other queues,
                // or is equal to the avg. after this, the final
                // queue will have at max (num_queues - 1) jobs more than the
                // other queues
                //
                _add_to_queue(&queues[num_queues - 1], job_list);

                //
                // Update tail pointer and total_rounds
                //
                while (queues[num_queues - 1].tail->next != NULL)
                {
                    queues[num_queues - 1].tail = queues[num_queues - 1].tail->next;
                    queues[num_queues - 1].total_rounds += queues[num_queues - 1].tail->rounds;
                }
            }
        }

        for (i = 0; i < num_queues; ++i) {
            pthread_mutex_unlock(queues[i].mutex);
        }
    }

    pthread_exit( NULL );
}
