wrk.method = "POST"

wrk.body = [[
{
  "name": "Jack",
  "age": 22,
  "email": "jack@gmail.com"
}
]]

wrk.headers["Content-Type"] = "application/json"