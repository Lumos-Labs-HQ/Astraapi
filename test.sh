echo "async"
wrk -t1 -c100 -d10s http://127.0.0.1:8002/async

echo "protected"
wrk -t1 -c100 -d10s -H "token: 123"  http://localhost:8002/protected

echo "check token"
wrk -t1 -c100 -d10s -H "token: 123"  http://localhost:8002/check


echo "di"
wrk -t1 -c100 -d10s -H "token: 1234"  http://localhost:8002/di

echo "nested di"
wrk -t1 -c100 -d10s -H "token: 1234"  http://localhost:8002/nested-di

echo "pydantic"
wrk -t1 -c100 -d10s -s post.lua  http://127.0.0.1:8002/users


echo "pydantic nested"
wrk -t1 -c100 -d10s -s nested.lua  http://127.0.0.1:8002/nested-users


echo "all in one"
wrk -t1 -c100 -d10s -s all-in-one.lua  http://127.0.0.1:8002/all-in-one

