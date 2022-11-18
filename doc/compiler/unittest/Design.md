<!--
Copyright (c) 2022, 2022 IBM Corp. and others

This program and the accompanying materials are made available under
the terms of the Eclipse Public License 2.0 which accompanies this
distribution and is available at https://www.eclipse.org/legal/epl-2.0/
or the Apache License, Version 2.0 which accompanies this distribution and
is available at https://www.apache.org/licenses/LICENSE-2.0.

This Source Code may also be made available under the following
Secondary Licenses when the conditions for such availability set
forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
General Public License, version 2 with the GNU Classpath
Exception [1] and GNU General Public License, version 2 with the
OpenJDK Assembly Exception [2].

[1] https://www.gnu.org/software/classpath/license.html
[2] https://openjdk.org/legal/assembly-exception.html

SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
-->

# Types of tests
Different parts of the compiler have different notions of what a unit is. In the optimizer, a better notion of a unit might be running a specific optimization, generating code, running the code, and validating the functionality. The codegen on the other hand, might only involve giving an evaluator a specific set of IL and validating the instruction stream it generates.

## Test API Contract
1. Build up necessary environment
2. Invoke API to test.
3. Validate the contract, either via the return value or the side effect.

## Test Transformation
1. Build up necessary environment
2. Invoke code path to test (e.g., an optimization)
3. Validate the tranformation; there are two approaches:
    1. Check the transformation by ensuring the IL looks as expected.
    2. Send the transformed IL to the codegen to generate code, and then run the code.

# Requirements
* A means to encapsulate a unit test.
* A framework to invoke the unit tests, and keep statistics.
* A means to set up the compiler environment.
* A means to actually execute compiled code.
* Automatic IL Generation

# Implementation Options
Use philosopy of [1], namely:
1. Start the JVM with options to compile nothing, and have it run Java code.
2. The Java code

## Levels of "Frameworks"
1. Java code framework to coordinate with JVM:
    * Wait for API Contract Unit Tests
    * Wait for Transformation Unit Tests to generate code and then run it
        * This can be done via an empty method (from a java point of view) that the compiler hijacks to generate code for.
        * Coordination can be done via a static field or other well know object.
        * Empty method should have a well defined signature so that results can be conveyed back to the C++ test framwork.
2. C++ test framework in the compiler to invoke unit tests.
    * Keep track of results from the C++ Unit Test, or coordinate with the Java Framework
3. C++ unit test template:
    * Specifies how the test should be run
    * Does the testing & validation if needed; otherwise, does the "compilation" for use by the Java Framework
    * Well defined APIs such that the C++ Framework can iterate over and invoke.

## Automatic IL Generation
* Can use TRIL to specify IL
    * TRIL can be specified in the annotations of the Java code that coordinates with the C++ Framework.

## Open Questions and Considerations
* Can we generate code for the same empty java method over and over again?
    * Investigate AVL Trees coordination.
* Can something like gtest be used for the C++ Framework?
    * How to coordinate with executing Java code?
    * How to keep it consistent with API Contract tests?
    * Look at `omr/fvtest/compilertest/tests/main.cpp` for an idea on how to integrate.
* How to deal with running with different `-Xjit`/`-Xaot` options?
    * Does asking this question even make sense in the context of unit testing?
    * Perhaps a test can just set the option, or perhaps the C++ Framework can have a notion of "variations"

# Design

## API Contract Unit Test Workflow
```
     Java Thread           Test Thread
+------------------+  +------------------+
|                  |  |                  |
|   Set State to   |  |    Wait until    |
|      TEST        |  |   State in Java  |
|                  |  |    Object is     |
|    Wait until    |  |       TEST       |
|   State change   |  |                  |
|        |         |  |      Run API     |
|        |         |  |  Contract Tests  |
|        |         |  |        |         |
|        |         |  |        |         |
|        |         |  |        |         |
|        |         |  |        |         |
|        |         |  |        |         |
|        |         |  |        |         |
|        |         |  |        v         |
|        |         |  |   Print Results  |
|        |         |  |                  |
|        |         |  |   Set State to   |
|        |         |  |      EXIT        |
|        v         |  |                  |
|       Exit       |  |                  |
+------------------+  +------------------+
```

## Transformation Unit Test Workflow
```
     Java Thread           Test Thread
+------------------+  +------------------+
|                  |  |                  |
|   Set State to   |  |    Wait until    |
|      TEST        |  |   State in Java  |
|                  |  |    Object is     |
|    Wait until    |  |       TEST       |
|   State change   |  |                  |
|        |         |  |       Run        |
|        |         |  | Transformation   |
|        |         |  |      Test        |
|        |         |  |                  |
|        |         |  |   Set State to   |
|        |         |  |      EXEC        |
|        v         |  |                  |
| Run Empty Method |  |    Wait until    |
|                  |  |   State change   |
|   Store Result   |  |        |         |
|                  |  |        |         |
|   Set State to   |  |        |         |
|      DONE        |  |        |         |
|                  |  |        |         |
|    Wait until    |  |        |         |
|   State change   |  |        |         |
|        |         |  |        v         |
|        |         |  |   Print Results  |
|        |         |  |                  |
|        |         |  |   Set State to   |
|        |         |  |      EXIT        |
|        |         |  |                  |
|        v         |  |                  |
|      Exit        |  |                  |
+------------------+  +------------------+
```

# Links
[1] https://github.com/eclipse-openj9/openj9/issues/16223
[2] https://github.com/eclipse/omr/issues/6799
