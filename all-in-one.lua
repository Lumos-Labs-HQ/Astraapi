wrk.method = "POST"

wrk.body = [[
{
  "name":"Jack",
  "age":22,
  "email":"jack@gmail.com",
  "address":{
    "city":"Kolkata",
    "country":"India"
  }
}
]]

wrk.headers["Content-Type"] = "application/json"
wrk.headers["token"] = "1234"