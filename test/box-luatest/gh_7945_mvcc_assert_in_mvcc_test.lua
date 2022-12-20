local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all = function()
    g.server = server:new{
        alias   = 'default',
        box_cfg = {memtx_use_mvcc_engine = true}
    }
    g.server:start()
end

g.after_all = function()
    g.server:drop()
end

g.before_each(function()
    g.server:exec(function()
        local s = box.schema.space.create('test')
    end)
end)

g.after_each(function()
    g.server:exec(function()
        if box.space.test then
            box.space.test:drop()
        end
    end)
end)

-- Test that checks that a read tracker that was set after gap write does not
-- get in the way after the reader is committed.
g.test_commit_after_gap_write = function()
    g.server:exec(function()
        box.space.test:create_index('primary', {type = 'tree'})

        local t = require('luatest')
        local txn_proxy = require('test.box.lua.txn_proxy')
        local fiber = require('fiber')

        box.space.test:replace{10}

        local tx1 = txn_proxy.new()
        local tx2 = txn_proxy.new()

        tx1:begin()
        tx2:begin()

        -- Set tx1 to track interval from minus infinity to {10}.
        tx1("box.space.test:select({}, {limit=1})")
        -- tx2 breaks interval into (-infinity, 2) and (2, 10), setting read
        -- tracker for tx1 and tuple {2}.
        tx2("box.space.test:replace{2}")
        -- Make tx1 to be RW.
        tx1("box.space.test:replace{1}")

        -- Commit both in certain order.
        local res1,msg1,res2,msg2
        local f1 = fiber.create(function()
            fiber.self():set_joinable(true)
            res1,msg1 = pcall(tx1.commit, tx1)
        end)
        local f2 = fiber.create(function()
            fiber.self():set_joinable(true)
            res2,msg2 = pcall(tx2.commit, tx2)
        end)
        f1:join()
        f2:join()

        t.assert_equals(res1, true)
        t.assert_equals(msg1, "")
        t.assert_equals(res2, true)
        t.assert_equals(msg2, "")

        t.assert_equals(box.space.test:select{}, {{1}, {2}, {10}})
    end)
end

-- Test that checks that a read tracker that was set full scan does not
-- get in the way after the reader is committed.
-- This tests is quite the same as the previous but it check a bit different
-- execution workflow, see the comments.
g.test_commit_after_full_scan = function()
    g.server:exec(function()
        box.space.test:create_index('primary', {type = 'hash'})

        local t = require('luatest')
        local txn_proxy = require('test.box.lua.txn_proxy')
        local fiber = require('fiber')

        local tx1 = txn_proxy.new()
        local tx2 = txn_proxy.new()

        tx1:begin()
        tx2:begin()

        -- Set tx1 to track full scan.
        tx1("box.space.test:select{}")
        -- tx2 breaks full scan, setting read tracker for tx1 and tuple {2}.
        tx2("box.space.test:replace{2}")
        -- Make tx1 to be RW.
        tx1("box.space.test:replace{1}")

        -- Commit both in certain order.
        local res1,msg1,res2,msg2
        local f1 = fiber.create(function()
            fiber.self():set_joinable(true)
            res1,msg1 = pcall(tx1.commit, tx1)
        end)
        local f2 = fiber.create(function()
            fiber.self():set_joinable(true)
            res2,msg2 = pcall(tx2.commit, tx2)
        end)
        f1:join()
        f2:join()

        t.assert_equals(res1, true)
        t.assert_equals(msg1, "")
        t.assert_equals(res2, true)
        t.assert_equals(msg2, "")

        t.assert_equals(box.space.test:select{}, {{1}, {2}})
    end)
end
