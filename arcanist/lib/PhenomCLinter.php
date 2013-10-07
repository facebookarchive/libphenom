<?php
/*
 * Copyright 2013-present Facebook, Inc.
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

class PhenomCLinter extends ArcanistLinter {
  private $futures = array();

  public function willLintPaths(array $paths) {
    $working_copy = $this->getEngine()->getWorkingCopy();
    $root = $working_copy->getProjectRoot();
    $chunks = array_chunk($paths, 8);
    foreach ($chunks as $chunk) {
      $f = new ExecFuture(
        "python %C --root=include --filter=-build/include_order ".
        "--verbose=2 %Ls", $root . '/thirdparty/cpplint.py', $chunk);
      $f->start();
      $this->futures[] = $f;
    }
  }

  public function getLinterName() {
    return 'clint.sh';
  }

  private function collectResults() {
    while ($this->futures) {
      $f = array_shift($this->futures);

      list($err, $stdout, $stderr) = $f->resolve();

      $lines = explode("\n", $stderr);
      $messages = array();
      foreach ($lines as $line) {
        $line = trim($line);
        $matches = null;
        $regex = '/^([^:]+):(\d+):\s*(.*)\s*\[(.*)\] \[(\d+)\]$/';
        if (!preg_match($regex, $line, $matches)) {
          continue;
        }
        foreach ($matches as $key => $match) {
          $matches[$key] = trim($match);
        }

        $message = new ArcanistLintMessage();
        $message->setPath($matches[1]);
        $message->setLine($matches[2]);
        $message->setCode($matches[4]);
        $message->setName($matches[4]);
        $message->setDescription($matches[3]);
        $message->setSeverity(
          $matches[5] >= 4 ?
          ArcanistLintSeverity::SEVERITY_ERROR :
          ArcanistLintSeverity::SEVERITY_WARNING
        );
        $this->addLintMessage($message);
      }
    }
  }

  public function lintPath($path) {
    $this->collectResults();
  }
}


// vim:ts=2:sw=2:et:

