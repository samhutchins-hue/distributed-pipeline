/* Copyright (c) 2010-2025. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "simgrid/s4u.hpp"
namespace sg4 = simgrid::s4u;

static const int SCALE_OUT_THRESHOLD = 10;
static const int SCALE_IN_THRESHOLD = 5;
static const int MIN_SECOND_STAGE_WORKERS = 2;
static const int MAX_SECOND_STAGE_WORKERS = 10;

static int currentSecondStageWorkers = 2;

struct Task {
  int id;
  double cost;
  bool terminate;
};

XBT_LOG_NEW_DEFAULT_CATEGORY(pipeline,
                             "Messages specific for this s4u example");

bool loadSpikeTriggerd = false;
static void master(std::vector<std::string> args) {
  xbt_assert(args.size() > 3,
             "The master function expects at least 3 arguments");

  long simulation_timeout = std::stol(args[1]);
  double compute_cost = std::stod(args[2]);
  double communication_cost = std::stod(args[3]);

  // get the first stage queue
  XBT_INFO("setting first host");
  sg4::MessageQueue *first_stage_queue =
      sg4::MessageQueue::by_name("first_stage");

  std::vector<sg4::Host *> all_hosts =
      sg4::Engine::get_instance()->get_all_hosts();

  sg4::Host *my_host = sg4::this_actor::get_host();

  double start_time = sg4::Engine::get_clock();
  double current_time = start_time;
  int tasks_dispatched = 0;

  // task id
  int task_id = 0;

  // TODO: logging tasks_dispatched
  while (current_time - start_time < simulation_timeout) {

    // Allocate a new task with a unique id and the compute cost
    Task *task = new Task{task_id, compute_cost};

    XBT_INFO("Master dispatching task ID %d with cost %g", task_id,
             compute_cost);

    first_stage_queue->put(task, communication_cost);
    tasks_dispatched++;
    task_id++;

    current_time = sg4::Engine::get_clock();

    if (!loadSpikeTriggerd && current_time >= simulation_timeout / 2.0) {
      loadSpikeTriggerd = true;
      XBT_INFO("HEAVY LOAD --------------------");
      for (size_t count = 0; count < 10; count++) {
        Task *task = new Task{task_id, compute_cost};
        XBT_INFO("Master dispatching task ID %d with cost %g", task_id,
                 compute_cost);
        first_stage_queue->put(task, communication_cost);
        tasks_dispatched++;
        task_id++;
      }
    }
  }

  XBT_INFO(
      "Simulation timeout reached. %d tasks were processed in %.1f seconds.",
      tasks_dispatched, current_time - start_time);

  Task *poison = new Task{-1, -1.0};
  XBT_INFO("Forwarding poison pill to first stage");
  first_stage_queue->put(poison);

  // sg4::MessageQueue *second_stage_queue =
  //     sg4::MessageQueue::by_name("second_stage");

  // size_t actor_count = sg4::Engine::get_instance()->get_actor_count();
  // for (size_t i = 0; i < actor_count; i++) {
  //   Task *poison = new Task{-1, 0, true};
  //   second_stage_queue->put(poison);
  // }

  XBT_INFO("Master done.");
}

void setup_host_profiles() {}

// TODO:
//  control dynamic actor creation
void control_actor() {}

static void first_stage() {
  sg4::Host *my_host = sg4::this_actor::get_host();
  sg4::MessageQueue *my_queue = sg4::MessageQueue::by_name("first_stage");
  sg4::MessageQueue *next_queue = sg4::MessageQueue::by_name("second_stage");

  bool running = true;
  while (running) {
    Task *task = my_queue->get<Task>();

    if (task->id == -1) {
      running = false;
      next_queue->put(task);
      continue;
    }

    XBT_INFO("Worker on %s got task ID %d with cost %g", my_host->get_cname(),
             task->id, task->cost);

    sg4::this_actor::execute(task->cost);

    XBT_INFO("Worker on %s finished task ID %d , forwarding to stage 2",
             my_host->get_cname(), task->id);

    next_queue->put(task);
  }

  XBT_INFO("Worker on %s received poison pill. Exiting.", my_host->get_cname());
}

static void second_stage() {
  sg4::Host *my_host = sg4::this_actor::get_host();
  sg4::MessageQueue *my_queue = sg4::MessageQueue::by_name("second_stage");
  sg4::MessageQueue *next_queue = sg4::MessageQueue::by_name("third_stage");

  bool running = true;
  while (running) {
    Task *task = my_queue->get<Task>();

    if (task->id == -1) {
      running = false;
      next_queue->put(new Task{-1, -1, true});
      delete task;
      continue;
    }

    XBT_INFO("Worker on %s got task ID %d with cost %g", my_host->get_cname(),
             task->id, task->cost);

    sg4::this_actor::execute(task->cost);

    XBT_INFO("Worker on %s finished task ID %d, forwarding to stage 3",
             my_host->get_cname(), task->id);

    next_queue->put(task);
  }

  XBT_INFO("Worker on %s received poison pill. Exiting.", my_host->get_cname());
}

static void third_stage() {
  sg4::Host *my_host = sg4::this_actor::get_host();
  sg4::MessageQueue *my_queue = sg4::MessageQueue::by_name("third_stage");

  bool running = true;
  while (running) {
    Task *task = my_queue->get<Task>();

    if (task->id == -1) {
      running = false;

      // TODO: log poison pill
      delete task;
      continue;
    }

    XBT_INFO("Worker on %s got task ID %d with cost %g", my_host->get_cname(),
             task->id, task->cost);

    sg4::this_actor::execute(task->cost);

    delete task;
  }

  XBT_INFO("Worker on %s received poison pill. Exiting.", my_host->get_cname());
}

static void autoscaler() {
  // retreive the second stage messageQueue
  sg4::MessageQueue *stage2Queue = sg4::MessageQueue::by_name("second_stage");

  while (true) {
    // sleep for a short period between checking for scaling
    sg4::this_actor::sleep_for(0.2);

    // queue size
    size_t qlen = stage2Queue->size();

    // scale out: spawn a new second stage worker if queue length exceeds set
    // range
    if (qlen >= SCALE_OUT_THRESHOLD &&
        currentSecondStageWorkers < MAX_SECOND_STAGE_WORKERS) {
      std::string actor_name = "second_stage_dynamic_" +
                               std::to_string(currentSecondStageWorkers + 1);
      sg4::Engine::get_instance()->add_actor(
          actor_name, sg4::Engine::get_instance()->host_by_name("second_stage"),
          second_stage);
      ++currentSecondStageWorkers;
      XBT_INFO(
          "Autoscaler: Scaling OUT, new worker count = %d (queue size = %zd)",
          currentSecondStageWorkers, qlen);
    }
    // scale in: reduce second stage workers if the queue length is low
    else if (qlen < SCALE_OUT_THRESHOLD &&
             currentSecondStageWorkers > MIN_SECOND_STAGE_WORKERS) {
      // second signal to terminate one worker
      sg4::MessageQueue::by_name("second_stage")->put(new Task{-1, 0, true});
      --currentSecondStageWorkers;
      XBT_INFO(
          "Autoscsaler: Scaling IN, new worker count = %d (queue size = %zd)",
          currentSecondStageWorkers, qlen);
    }
  }
  XBT_INFO("Autoscaler: Exiting.");
}

int main(int argc, char *argv[]) {
  sg4::Engine e(&argc, argv);

  // register the master function
  e.register_function("master", &master);

  e.load_platform(argv[1]);
  e.load_deployment(argv[2]);

  // first stage workers
  e.add_actor("first_stage", e.host_by_name("first_stage"), first_stage);
  // second stage workers
  e.add_actor("second_stage_1", e.host_by_name("second_stage"), second_stage);
  // e.add_actor("second_stage_2", e.host_by_name("second_stage"),
  // second_stage);
  //
  // currentSecondStageWorkers = 2;
  //
  //  e.add_actor("second_stage_2", e.host_by_name("second_stage"),
  //  second_stage);
  //   third stage workers
  e.add_actor("third_stage", e.host_by_name("third_stage"), third_stage);
  // e.add_actor("autoscaler", e.host_by_name("master"), autoscaler);

  // run the simulation
  e.run();

  XBT_INFO("Simulation is over");

  return 0;
}
