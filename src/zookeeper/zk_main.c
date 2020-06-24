#include "../../include/mica/kvs.h"
#include "../../include/zookeeper/zk_util.h"
#include "../../include/zookeeper/zk_main.h"
#include "../../include/general_util/generic_func.h"


//Global Vars
struct latency_counters latency_count;
struct thread_stats t_stats[LEADERS_PER_MACHINE];
remote_qp_t remote_follower_qp[FOLLOWER_MACHINE_NUM][FOLLOWERS_PER_MACHINE][FOLLOWER_QP_NUM];
remote_qp_t remote_leader_qp[LEADERS_PER_MACHINE][LEADER_QP_NUM];
atomic_bool qps_are_set_up;
atomic_uint_fast64_t global_w_id, committed_global_w_id;
bool is_leader;

int main(int argc, char *argv[])
{
  zk_print_parameters_in_the_start();
  static_assert_compile_parameters();
	init_globals(QP_NUM);
  zk_init_globals();



	int i;

  handle_program_inputs(argc, argv);



	struct thread_params *param_arr;
	/* Launch leader/follower threads */

	is_leader = machine_id == LEADER_MACHINE;
	num_threads =  is_leader ? LEADERS_PER_MACHINE : FOLLOWERS_PER_MACHINE;
	param_arr = malloc(num_threads * sizeof(struct thread_params));
  pthread_t * thread_arr = malloc(num_threads * sizeof(pthread_t));

	pthread_attr_t attr;
	cpu_set_t pinned_hw_threads;
	pthread_attr_init(&attr);
	bool occupied_cores[TOTAL_CORES] = { 0 };
  char node_purpose[15];
  void *thread_func;
  if (is_leader) {
    sprintf(node_purpose, "Leader");
    thread_func = leader;
  }
  else {
    sprintf(node_purpose, "Follower");
    thread_func = follower;
  }
	for(i = 0; i < TOTAL_THREADS; i++) {
    if (i < WORKERS_PER_MACHINE) {
      spawn_threads(param_arr, i, node_purpose, &pinned_hw_threads,
                    &attr, thread_arr, thread_func, occupied_cores);
    }
    else {
      assert(ENABLE_CLIENTS);
      fopen_client_logs(i);
      spawn_threads(param_arr, i, "Client", &pinned_hw_threads,
                    &attr, thread_arr, client, occupied_cores);
    }
	}


	for(i = 0; i < num_threads; i++)
		pthread_join(thread_arr[i], NULL);

	return 0;
}