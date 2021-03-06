/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import java.lang.reflect.Method;

public class Main {
  private static int runTestCase(String name, long arg) throws Exception {
    Class<?> c = Class.forName("TestCase");
    Method m = c.getMethod(name, long.class);
    int result = (Integer) m.invoke(null, arg);
    return result;
  }

  private static void assertEquals(int expected, int actual) {
    if (expected != actual) {
      throw new Error("Wrong result: " + expected + " != " + actual);
    }
  }

  public static void main(String[] args) throws Exception {
    assertEquals(42, runTestCase("invalidateLow", 42L));
  }
}
