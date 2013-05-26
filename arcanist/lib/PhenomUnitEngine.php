<?php

/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

class PhenomUnitEngine extends ArcanistBaseUnitTestEngine {
  private $projectRoot;

  protected function supportsRunAllTests() {
    return true;
  }

  protected function getProjectRoot() {
    if (!$this->projectRoot) {
      $this->projectRoot = $this->getWorkingCopy()->getProjectRoot();
    }
    return $this->projectRoot;
  }

  protected function make($target) {
    return execx("cd %s && make %s",
      $this->getProjectRoot(), $target);
  }

  public function run() {
    return $this->runUnitTests();
  }

  private function stripName($filename) {
    $name = basename($filename);
    $name = preg_replace("/\..*$/", '', $name);
    return $name;
  }

  private function getTestsToRun() {
    $root = $this->getProjectRoot();
    $test_dir = $root . "/tests/";
    $tests = array();
    foreach (glob($test_dir . "*.t") as $test) {
      $relname = substr($test, strlen($test_dir));
      $tests[$relname] = $test;
    }
    if (!$this->getRunAllTests()) {
      /* run tests that sound similar to the modified paths */

      printf("Finding tests based on name similarity\n");
      printf("Use `arc unit --everything` to run them all\n");

      $paths = array();
      foreach ($this->getPaths() as $path) {
        $paths[] = $this->stripName($path);
      }

      foreach ($tests as $relname => $test) {
        $keep = false;

        $strip = $this->stripName($relname);
        foreach ($paths as $path) {
          $pct = 0;
          similar_text($path, $strip, $pct);
          if ($pct > 55) {
            $keep = true;
            break;
          }
        }
        if (!$keep) {
          unset($tests[$relname]);
        }
      }

    }

    return $tests;
  }

  public function runUnitTests() {
    // Build any unit tests
    $this->make('build-tests');

    // Now find all the test programs
    $root = $this->getProjectRoot();
    $futures = array();
    $cg_files = array();
    $obj_files = array();
    $coverage = $this->getEnableCoverage();

    $tests = $this->getTestsToRun();
    foreach ($tests as $relname => $test) {
      if ($coverage) {
        $cg = new TempFile();
        $cg_files[$relname] = $cg;
        $obj_files[] = $test;
        $test =
          'valgrind --tool=callgrind --collect-jumps=yes ' .
          '--callgrind-out-file=' . $cg . ' ' . $test;
      }

      $futures[$relname] = new ExecFuture($test);
    }

    $results = array();
    // Use a smaller limit for SunOS because my test VM has crappy
    // timing characteristics and that causes timer.t to fail
    $limit = PHP_OS == 'SunOS' ? 1 : 4;
    foreach (Futures($futures)->limit($limit) as $test => $future) {
      list($err, $stdout, $stderr) = $future->resolve();

      $results[$test] = $this->parseTestResults(
        $test, $err, $stdout, $stderr);
    }

    if ($coverage) {
      // Find all executables, not just the test executables.
      // We need to do this because the test executables may not
      // include all of the possible code (linker decides to omit
      // it from the image) so we see a skewed representation of
      // the source lines.

      $fp = popen(
        "find $root -type f -name \*.a -o -name \*.so -o -name \*.dylib",
        "r");
      while (true) {
        $line = fgets($fp);
        if ($line === false) break;
        $obj_files[] = trim($line);
      }

      // Parse line information from the objects
      foreach ($obj_files as $object) {
        PhenomDwarfLineInfo::loadObject($object);
      }

      // Now process coverage data files
      foreach ($cg_files as $relname => $cg) {
        $CG = new PhenomCallgrindFile((string)$cg);
        $CG->parse();

        $source_files = array();
        foreach ($CG->getSourceFiles() as $filename) {
          if (Filesystem::isDescendant($filename, $root) &&
              !preg_match("/(thirdparty|tests)/", $filename)) {
            $source_files[$filename] = $filename;
          }
        }

        $cov = array();
        foreach ($source_files as $filename) {
          $relsrc = substr($filename, strlen($root) + 1);
          $cov[$relsrc] = PhenomCallgrindFile::mergeSourceLineData(
            $filename, array($CG));
        }
        // Update the coverage data for that test.
        // arcanist will merge the final results
        $results[$relname]->setCoverage($cov);
      }
    }

    return $results;
  }

  private function parseTestResults($test, $err, $stdout, $stderr) {
    $result = new ArcanistUnitTestResult();
    $result->setName($test);
    $result->setUserData($stdout . $stderr);
    $result->setResult($err == 0 ?
      ArcanistUnitTestResult::RESULT_PASS :
      ArcanistUnitTestResult::RESULT_FAIL
    );
    if (preg_match("/# ELAPSED: (\d+)ms/", $stderr, $M)) {
      $result->setDuration($M[1] / 1000);
    }

    return $result;
  }
}

// vim:ts=2:sw=2:et:
