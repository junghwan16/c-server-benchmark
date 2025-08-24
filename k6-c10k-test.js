import http from 'k6/http';
import { check, sleep } from 'k6';
import { Rate } from 'k6/metrics';

const errorRate = new Rate('errors');

export const options = {
    scenarios: {
        // Gradual ramp-up to C10K
        c10k_test: {
            executor: 'ramping-vus',
            startVUs: 10,
            stages: [
                { duration: '10s', target: 100 },    // Warm up to 100
                { duration: '20s', target: 500 },    // Ramp to 500
                { duration: '20s', target: 1000 },   // Ramp to 1000
                { duration: '20s', target: 2000 },   // Ramp to 2000
                { duration: '20s', target: 5000 },   // Ramp to 5000
                { duration: '30s', target: 10000 },  // Ramp to 10000 (C10K)
                { duration: '30s', target: 10000 },  // Stay at 10000
                { duration: '20s', target: 0 },      // Ramp down
            ],
        },
    },
    thresholds: {
        http_req_duration: ['p(95)<500'], // 95% of requests should be below 500ms
        errors: ['rate<0.1'],              // Error rate should be below 10%
    },
};

export default function () {
    const response = http.get('http://localhost:8080/index.html', {
        timeout: '10s',
    });
    
    const success = check(response, {
        'status is 200': (r) => r.status === 200,
        'response time < 500ms': (r) => r.timings.duration < 500,
    });
    
    errorRate.add(!success);
    
    // Small random sleep between requests
    sleep(Math.random() * 0.1);
}

export function handleSummary(data) {
    return {
        'stdout': textSummary(data, { indent: ' ', enableColors: true }),
        'results/k6-c10k-summary.json': JSON.stringify(data),
    };
}

// Helper function for text summary
function textSummary(data, options) {
    const indent = options.indent || '';
    const output = [];
    
    output.push('\n=== K6 C10K Test Results ===\n');
    
    // VUs (Virtual Users)
    if (data.metrics.vus) {
        output.push(`${indent}Max VUs: ${data.metrics.vus.max}`);
    }
    
    // Request metrics
    if (data.metrics.http_reqs) {
        output.push(`${indent}Total Requests: ${data.metrics.http_reqs.count}`);
        output.push(`${indent}Request Rate: ${data.metrics.http_reqs.rate.toFixed(2)}/s`);
    }
    
    // Response time
    if (data.metrics.http_req_duration) {
        const duration = data.metrics.http_req_duration;
        output.push(`${indent}Response Times:`);
        output.push(`${indent}  min: ${duration.min.toFixed(2)}ms`);
        output.push(`${indent}  med: ${duration.med.toFixed(2)}ms`);
        output.push(`${indent}  avg: ${duration.avg.toFixed(2)}ms`);
        output.push(`${indent}  p95: ${duration['p(95)'].toFixed(2)}ms`);
        output.push(`${indent}  p99: ${duration['p(99)'].toFixed(2)}ms`);
        output.push(`${indent}  max: ${duration.max.toFixed(2)}ms`);
    }
    
    // Error rate
    if (data.metrics.errors) {
        output.push(`${indent}Error Rate: ${(data.metrics.errors.rate * 100).toFixed(2)}%`);
    }
    
    // Failed requests
    if (data.metrics.http_req_failed) {
        output.push(`${indent}Failed Requests: ${data.metrics.http_req_failed.passes}/${data.metrics.http_req_failed.count}`);
    }
    
    return output.join('\n') + '\n';
}