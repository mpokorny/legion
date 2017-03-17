-- Copyright 2017 Stanford University
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

local r = regentlib.newsymbol(region(int), "r")
local reads_r = regentlib.privilege(regentlib.reads, r)
task f([r])
where [reads_r] do
  var t = 0
  for x in r do
    t += @x
  end
  return t
end

task main()
  var r = region(ispace(ptr, 5), int)
  new(ptr(int, r), 3)
  fill(r, 10)
  regentlib.assert(f(r) == 30, "test failed")
end
regentlib.start(main)
