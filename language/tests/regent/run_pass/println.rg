-- Copyright 2020 Stanford University
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

import "regent"

local format = require("std/format")

task main()
  var x : int32 = 1
  var y : uint64 = 1234
  format.println("Hello {} {} world!", x, y)

  var z : float = 1.23
  var w : double = 3.45
  format.println("Floats: {} {}", z, w)

  var i1 = int1d(1)
  var i2 = int2d { 1, 2 }
  var i3 = int3d { 1, 2, 3 }
  format.println("int1d {}", i1)
  format.println("int2d {}", i2)
  format.println("int3d {}", i3)

  var is = ispace(int2d, { 2, 2 })
  var r = region(is, int)
  for i in is do
    format.println("int2d(is) {}", i)
  end
  for x in r do
    format.println("int2d(r) {}", x)
  end

  format.println("rect2d {}", is.bounds)

  -- Regent's println *DOES NOT* follow C's printf codes, so this
  -- should just print the literal string.
  format.println("%d %f %s %%")
end
regentlib.start(main)