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

/* Uses `dwarfdump` to determine which lines are executable in
 * a given object file or set of object files.
 * No need to parse the sources.
 */

class PhenomDwarfLineInfo {
  public $filename;
  public $lines_by_file = array();
  static $cache = array();

  static function loadObject($filename) {
    if (isset(self::$cache[$filename])) {
      return self::$cache[$filename];
    }
    $D = new PhenomDwarfLineInfo($filename);
    $D->parse();
    self::$cache[$filename] = $D;
    return $D;
  }

  static function mergedSourceLines($filename) {
    $lines = array();
    foreach (self::$cache as $D) {
      if (!isset($D->lines_by_file[$filename])) continue;
      foreach ($D->lines_by_file[$filename] as $line) {
        $lines[$line] = $line;
      }
    }
    return $lines;
  }

  function __construct($filename) {
    $this->filename = $filename;
  }

  function parse() {
    $fp = popen(sprintf("dwarfdump -l %s",
            escapeshellarg($this->filename)), 'r');

    $sourcefile = null;
    while (true) {
      $line = fgets($fp);
      if ($line === false) break;

      // 0x004021d0  [  53, 0] NS uri: "/mnt/hgfs/wez/fb/phenom/tests/memory.c"
      if (preg_match("/^0x[0-9a-fA-F]+\s+\[\s*(\d+),\s*\d+\]/", $line, $M)) {
        $exe_line = $M[1];

        if (preg_match('/uri: "([^"]+)"/', $line, $M)) {
          $sourcefile = $M[1];


          if (!isset($this->lines_by_file[$sourcefile])) {
            $this->lines_by_file[$sourcefile] = array();
          }
        }
        $this->lines_by_file[$sourcefile][$exe_line] = $exe_line;
      }
    }
  }
}

// vim:ts=2:sw=2:et:
