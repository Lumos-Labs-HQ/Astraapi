import http from 'k6/http';
import { check } from 'k6';

export const options = {
  discardResponseBodies: true,
  scenarios: {
    power_test: {
      executor: 'constant-arrival-rate',
      rate: 20000,        
      timeUnit: '1s',
      duration: '30s',

      preAllocatedVUs: 20000,
      maxVUs: 20000,
    },
  },
};

export default function () {
  let response = http.get('http://localhost:8002');
  check(response, {
    'status is 200': (r) => r.status === 200,
  });
}
