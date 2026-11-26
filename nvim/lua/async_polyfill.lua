-- Minimal vim.async polyfill for Neovim < 0.12 (supports run and await).
if vim.async then
  return
end

local M = {}

local function pack(...)
  return { n = select("#", ...), ... }
end

local function resume_co(co, ...)
  local ok, err = coroutine.resume(co, ...)
  if not ok then
    vim.schedule(function()
      vim.notify(tostring(err), vim.log.levels.ERROR)
    end)
  end
end

function M.run(fn)
  local task = { done = false, results = nil, waiters = {} }
  local co = coroutine.create(function()
    task.results = pack(fn())
    task.done = true
    for _, waiter in ipairs(task.waiters) do
      waiter()
    end
  end)
  resume_co(co)
  return task
end

function M.await(arg1, fn, ...)
  local co = coroutine.running()
  assert(co, "vim.async.await must be called inside vim.async.run")

  if type(arg1) == "table" then
    if not arg1.done then
      table.insert(arg1.waiters, function()
        resume_co(co)
      end)
      coroutine.yield()
    end
    return unpack(arg1.results, 1, arg1.results.n)
  end

  local argc = arg1
  local args = pack(...)
  local res = nil
  local called = false
  local yielded = false

  args[argc] = function(...)
    res = pack(...)
    if not called then
      called = true
      if yielded then
        resume_co(co)
      end
    end
  end

  fn(unpack(args, 1, argc))
  if not called then
    yielded = true
    coroutine.yield()
  end
  return unpack(res, 1, res.n)
end

vim.async = M
