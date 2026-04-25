local M = {}

function M.cpp_proto_header(fname)
  local root = vim.fn.fnamemodify(fname, ':r:r')
  local ext = vim.fn.fnamemodify(fname, ':e:e')
  if ext == 'pb.h' or ext == 'proto.h' then
    return root .. '.proto'
  end
  return fname
end

function M.gcl_fold()
  local start_num = vim.fn.nextnonblank(vim.v.foldstart)
  local end_num = vim.fn.prevnonblank(vim.v.foldend)

  local content = ''
  if end_num > start_num + 1 then
    content = '...'
    local lines = vim.fn.getline(start_num + 1, end_num - 1)
    for _, line in ipairs(lines) do
      local content_match = line:match("^%s*name = (.*)$")
      if content_match then
        content = content_match
        break
      end
    end
  end

  local start_text = vim.fn.getline(start_num)
  local end_text = vim.fn.substitute(vim.fn.getline(end_num), [[^\s*]], '', '')
  local text = start_text .. ' ' .. content .. ' ' .. end_text

  local width = vim.fn.winwidth(0) - vim.opt.foldcolumn:get() - (vim.opt.number:get() and vim.opt.numberwidth:get() or 0)
  local lines_folded = ' ' .. tostring(1 + vim.v.foldend - vim.v.foldstart) .. ' lines'

  text = vim.fn.substitute(text, [[\t]], string.rep(' ', vim.opt.tabstop:get()), 'g')
  text = vim.fn.strpart(text, 0, width - #lines_folded)
  local padding = string.rep(' ', width - #lines_folded - #text)
  return text .. padding .. lines_folded
end

function M.python_include_expr(arg)
  local result = vim.fn.substitute(
    vim.fn.substitute(arg, vim.b.grandparent_match, vim.b.grandparent_sub, ''),
    vim.b.parent_match, vim.b.parent_sub, '')

  result = vim.fn.substitute(result, ' as .*', '', '')
  result = vim.fn.substitute(result, ' import ', '.', '')
  result = vim.fn.substitute(result, [[^\(from\|import\) ]], '', 'g')
  return vim.fn.substitute(result, vim.b.child_match, vim.b.child_sub, 'g')
end

function M.bzl_include_expr(fname)
  local trimmed = fname:gsub("^/+", "")
  if not trimmed:find("/") then
    return vim.fn.expand("%")
  else
    return trimmed .. "/BUILD"
  end
end

return M
