import http from 'k6/http';
import { check } from 'k6';

export const options = {
  discardResponseBodies: true,
  scenarios: {
    power_test: {
      executor: 'constant-arrival-rate',
      rate: 300000,        
      timeUnit: '1s',
      duration: '10s',

      preAllocatedVUs: 20000,
      maxVUs: 300000,
    },
  },
};

export default function () {
  let response = http.get('http://localhost:8002/');
  check(response, {
    'status is 200': (r) => r.status === 200,
  });
}
