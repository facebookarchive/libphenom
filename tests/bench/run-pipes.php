#!/usr/bin/env php
<?php
// Collects data from iopipes.t for charting purposes.
// This runs some combination of sockets and threads and
// collects the data into /tmp/perf.csv
// You may then process that data file using
// `R --no-save --no-environ -f phenom.R` to generate
// sockets.png and scale.png to visualize libevent vs. libphenom
// and scaling vs. the number of NBIO threads in use

$all_combos = array(
  10, 100, 200, 300, 400, 500, 600, 700, 800, 900,
  1000, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900,
  2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000,
  11000, 12000, 13000, 14000, 15000, 16000, 17000, 18000,
  19000, 20000, 30000, 40000, 50000, 60000, 70000, 80000,
  90000, 100000
);

$fewer_combos = array(
  100, 1000, 2000, 4000, 8000, 10000,
  20000, 40000, 80000, 100000
);

$fds = $all_combos;

$time_limit = 5;
$num_tests = 0;
$start = microtime(true);
$file = "/tmp/perf.csv";
unlink($file);
$max_cores = 16;

foreach ($fds as $num_socks) {
  for ($num_cores = 1; $num_cores <= $max_cores; $num_cores++) {
    if ($num_cores == 1) {
      // Can only meaningfully run libevent tests with -c1
      passthru("APPEND_FILE=$file ./tests/bench/iopipes.t -e -n $num_socks -c 1 -t $time_limit");
      $num_tests++;
    }
    passthru("APPEND_FILE=$file ./tests/bench/iopipes.t -n $num_socks -c $num_cores -t $time_limit");
    $num_tests++;
  }
}

$end = microtime(true);
printf("Took %.3fs to run %d tests\n", $end-$start, $num_tests);

