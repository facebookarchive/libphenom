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

/* We need to override the default license linter so that
 * it doesn't keep flagging the BSD licensed code in
 * phenom/queue.h
 **/
class PhenomLicenseLinter extends ArcanistLicenseLinter {
  public $apache;
  public $holder;

  public $valid_holders = array(
    "The Regents of the University of California",
    "Lucent Technologies",
  );

  public function __construct() {
    $this->apache = new ArcanistApacheLicenseLinter();
  }

  public function getLinterName() {
    return 'PHENOMLICENSE';
  }

  protected function getLicenseText($copyright_holder) {
    $this->holder = $copyright_holder;
    return $this->apache->getLicenseText($copyright_holder);
  }

  protected function getLicensePatterns() {
    return $this->apache->getLicensePatterns();
  }

  public function addLintMessage(ArcanistLintMessage $msg) {
    $original = $msg->getOriginalText();
    if (!preg_match("/$this->holder/", $original)) {
      foreach ($this->valid_holders as $holder) {
        if (preg_match("/$holder/", $original)) {
          // Suppress this one
          return;
        }
      }
    }
    parent::addLintMessage($msg);
  }
}

// vim:ts=2:sw=2:et:

