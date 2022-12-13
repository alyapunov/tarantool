local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all = function()
    g.server = server:new{
        alias   = 'default',
        box_cfg = {
            memtx_use_mvcc_engine = true,
            txn_isolation = 'read-committed',
        }
    }
    g.server:start()
end

g.after_all = function()
    g.server:drop()
end

g.before_each(function()
    g.server:exec(function()
        local test = box.schema.create_space("test", {
            format = {{"id", type = "number"},
                      {"indexed", type = "string"}}
        })
        test:create_index("id", {unique = true, parts = {{1, "number"}}})
        test:create_index("indexed", {unique = false, parts = {{2, "string"}}})
    end)
end)

g.after_each(function()
    g.server:exec(function()
        if box.space.test then
            box.space.test:drop()
        end
    end)
end)

-- Test right from the issue, logically simplified.
g.test_excess_conflict_secondary = function()
    g.server:exec(function()
        local t = require('luatest')
        local fiber = require('fiber')
        local test = box.space.test

        local id = 0
        local nextId = function()
            id = id + 1;
            return id;
        end

        local fibers = {}
        local results = {}

        local put = function(fib_no)
            table.insert(fibers, fiber.self())
            fiber.self():set_joinable(true)
            box.begin()
            test:put({nextId(), "queue", "READY", "test" })
            results[fib_no] = pcall(box.commit)
        end

        local firstUpdate = function()
            table.insert(fibers, fiber.self())
            fiber.self():set_joinable(true)
            box.begin()
            for _, task in test.index.indexed:pairs() do
                test:update(task.id, { { '=', "indexed", "test + test" } })
            end
            results[3] = pcall(box.commit)
        end

        local secondUpdate = function()
            table.insert(fibers, fiber.self())
            fiber.self():set_joinable(true)
            results[4] = false
            box.begin()
            for _, task in test.index.indexed:pairs() do
                test:update(task.id, { { '=', "indexed", "test - test" } })
            end
            results[4] = pcall(box.commit)
        end

        for fib_no = 1, 2 do
            fiber.create(put, fib_no)
        end
        fiber.create(firstUpdate)
        fiber.create(secondUpdate)

        t.assert_equals(#fibers, 4)
        for _,f in pairs(fibers) do
            f:join()
        end

        t.assert_equals(results, {true, true, true, true})
        t.assert_equals(test:select{},
            {{1, 'test - test', 'READY', 'test'},
             {2, 'test - test', 'READY', 'test'}})
    end)
end
