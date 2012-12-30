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

/* Loads a data file produced by:
 * `valgrind --tool=callgrind --collect-jumps=yes`
 * and parses out information on which files were used
 * and which lines were executed */

class PhenomCallgrindFile {
  public $filename;
  public $names = array();
  public $function_costs = array();

  function __construct($filename) {
    $this->filename = $filename;
  }

  function parse() {
    $fp = fopen($this->filename, 'r');

    $last_line = null;
    $line = null;
    $last_file_id = 0;
    $fn_id = -1;
    $current_fn_line = 0;
    while (true) {
      $last_line = $line;
      $line = fgets($fp);
      if ($line === false) break;
      $line = rtrim($line, "\r\n");
      if (!strlen($line)) continue;

      if (preg_match("/^[a-z]*:.*/", $line)) continue;
      if (preg_match("/^([a-z]*)=\((\d*)\) (.*)/", $line, $M)) {
        list($_, $type, $id, $name) = $M;

        if (preg_match("/^([a-z]*)=\((\d*)\)/", $last_line, $M)) {
          list($_, $ptype, $pid) = $M;
        } else {
          $ptype = 'fl';
          $pid = $last_file_id;
        }

        $this->names[] = array(
          'type' => $type,
          'id' => $id,
          'name' => $name,
          'ptype' => $ptype,
          'pid' => $pid,
        );
      }

      if (preg_match("/^fl=\((\d*)\)/", $line, $M)) {
        $last_file_id = (int)$M[1];
        continue;
      }

      if (preg_match("/^fn=\((\d*)\)/", $line, $M)) {
        $fn_id = (int)$M[1];
        continue;
      }

      if ($fn_id == -1) {
        continue;
      }

      if ($line[0] == '*') continue;

      if (preg_match("/^\+(\d+)/", $line, $M)) {
        $current_fn_line += (int)$M[1];
      } else if (preg_match("/^\-(\d+)/", $line, $M)) {
        $current_fn_line -= (int)$M[1];
      } else if (preg_match("/^(\d+)/", $line, $M)) {
        $current_fn_line = (int)$M[1];
      }

      if (!isset($this->function_costs[$fn_id])) {
        $this->function_costs[$fn_id] = array(
          'lines' => array(),
          'jumps' => array(),
        );
      }
      if (!isset($this->function_costs[$fn_id]['lines'][$current_fn_line])) {
        $this->function_costs[$fn_id]['lines'][$current_fn_line]
                                                      = $current_fn_line;
      }

      if (preg_match('/^jump=(\d+) (.*)/', $line, $M)) {
        list($_, $count, $target) = $M;

        if (preg_match('/^\+(\d+)/', $target, $M)) {
          $to = $current_fn_line + (int)$M[1];
        } else if (preg_match('/^\-(\d+)/', $target, $M)) {
          $to = $current_fn_line - (int)$M[1];
        } else {
          $to = $current_fn_line;
        }

        $this->function_costs[$fn_id]['jumps'][] = array(
          'to' => $to,
          'to_count' => (int)$count,
          'from' => $current_fn_line,
          'from_count' => -1,
        );
      }

      if (preg_match('/^jcnd=(\d+)\/(\d+) (.*)/', $line, $M)) {
        list($_, $tocount, $fromcount, $target) = $M;

        if (preg_match('/^\+(\d+)/', $target, $M)) {
          $to = $current_fn_line + (int)$M[1];
        } else if (preg_match('/^\-(\d+)/', $target, $M)) {
          $to = $current_fn_line - (int)$M[1];
        } else {
          $to = $current_fn_line;
        }

        $this->function_costs[$fn_id]['jumps'][] = array(
          'to' => $to,
          'to_count' => (int)$tocount,
          'from' => $current_fn_line,
          'from_count' => (int)$fromcount - (int)$tocount,
        );
      }
    }
  }

  static function max(array $a) {
    if (!count($a)) {
      return 0;
    }
    return max($a);
  }

  static function computeCoverageString($sourcefile, array $touched_lines) {
    $exe_lines = PhenomDwarfLineInfo::mergedSourceLines($sourcefile);
    $max_line = max(self::max($exe_lines), self::max($touched_lines));

    /* now build a string containing the coverage info.
     * This is compatible with ArcanistUnitTestResult coverage data:
     * N not executable
     * C covered
     * U uncovered
     * X unreachable (we can't detect that here)
     */
    $cov = str_repeat('N', $max_line);
    for ($i = 1; $i <= $max_line; $i++) {
      if (isset($touched_lines[$i])) {
        $cov[$i - 1] = 'C';
      } else if (isset($exe_lines[$i])) {
        $cov[$i - 1] = 'U';
      }
    }

    return $cov;
  }

  static function mergeSourceLineData($sourcefile, array $cg_list) {
    $lines = array();
    foreach ($cg_list as $cg) {
      $cg->getTouchedLines($sourcefile, $lines);
    }

    return self::computeCoverageString($sourcefile, $lines);
  }

  function getTouchedLines($sourcefile, &$all_lines) {
    $type = null;
    $id = null;

    foreach ($this->names as $item) {
      if ($id === null) {
        if ($item['name'] == $sourcefile) {
          $id = $item['id'];
          $type = $item['type'];
        }
        continue;
      }

      if ($item['ptype'] == $type && $item['pid'] == $id &&
          ($item['type'] == 'fn' || $item['type'] == 'cfn')) {
        // Compute line coverage
        $fid = $item['id'];
        $lines = $this->function_costs[$fid]['lines'];

        foreach ($lines as $no) {
          $all_lines[$no] = $no;
        }
      }
    }
    return $all_lines;
  }

  function collectSourceFileData($sourcefile) {
    $all_lines = array();
    $this->getTouchedLines($sourcefile, $all_lines);

    foreach ($this->getObjectFiles() as $objfile) {
      PhenomDwarfLineInfo::loadObject($objfile);
    }
    $exe_lines = PhenomDwarfLineInfo::mergedSourceLines($sourcefile);
    $max_line = max($exe_lines);

    /* now build a string containing the coverage info.
     * This is compatible with ArcanistUnitTestResult coverage data:
     * N not executable
     * C covered
     * U uncovered
     * X unreachable (we can't detect that here)
     */
    $cov = str_repeat('N', $max_line);
    for ($i = 1; $i <= $max_line; $i++) {
      if (isset($all_lines[$i])) {
        $cov[$i - 1] = 'C';
      } else if (isset($exe_lines[$i])) {
        $cov[$i - 1] = 'U';
      }
    }

    return $cov;
  }

  /* print a source file with coverage annotation */
  function annotate($sourcefile) {
    $cov = $this->collectSourceFileData($sourcefile);
    $fp = fopen($sourcefile, 'r');
    $lineno = 0;
    while (true) {
      $line = fgets($fp);
      if ($line === false) break;
      $lineno++;
      $line = rtrim($line, "\r\n");

      $sigil = 'N';
      if (isset($cov[$lineno - 1])) {
        $sigil = $cov[$lineno - 1];
      }

      printf("%s %s\n", $sigil, $line);
    }
  }

  function getObjectFiles() {
    $files = array();
    foreach ($this->names as $item) {
      if ($item['type'] == 'ob') {
        $files[] = $item['name'];
      }
    }
    return $files;
  }

  function getSourceFiles() {
    $files = array();
    foreach ($this->names as $item) {
      switch ($item['type']) {
        case 'fl':
        case 'cfl':
        case 'fi':
        case 'cfi':
          $files[] = $item['name'];
      }
    }
    return $files;
  }
}



// vim:ts=2:sw=2:et:

