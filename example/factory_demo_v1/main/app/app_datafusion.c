#include <math.h>
#include "app_datafusion.h"

#define sampleFreq 25.0f          // sample frequency in Hz
#define twoKpDef   (2.0f * 1.0f)  // 2 * proportional gain
#define twoKiDef   (2.0f * 0.0f)  // 2 * integral gain
#define RAD_TO_DEG (180.0f / 3.14159265358979323846f)
#define DEG_TO_RAD (3.14159265358979323846f / 180.0f)

volatile float twoKp = twoKpDef;  // 2 * proportional gain (Kp)
volatile float twoKi = twoKiDef;  // 2 * integral gain (Ki)
volatile float
q0 = 1.0,
q1 = 0.0, q2 = 0.0,
q3 = 0.0;  // quaternion of sensor frame relative to auxiliary frame
volatile float integralFBx = 0.0f, integralFBy = 0.0f,
               integralFBz = 0.0f;  // integral error terms scaled by Ki

static float invSqrt(float x)
{
    float halfx = 0.5f * x;
    float y = x;
    long i = *(long *)&y;
    i = 0x5f3759df - (i >> 1);
    y = *(float *)&i;
    y = y * (1.5f - (halfx * y * y));
    return y;
}
void MahonyAHRSupdateIMU(float gx, float gy, float gz, float ax, float ay,
                         float az, bmi270_axis_t * axis_val)
{
    float recipNorm;
    float halfvx, halfvy, halfvz;
    float halfex, halfey, halfez;
    float qa, qb, qc;

    gx *= DEG_TO_RAD;
    gy *= DEG_TO_RAD;
    gz *= DEG_TO_RAD;
    // Compute feedback only if accelerometer measurement valid (avoids NaN in
    // accelerometer normalisation)
    if (!((ax == 0.0f) && (ay == 0.0f) && (az == 0.0f))) {
        // Normalise accelerometer measurement
        recipNorm = invSqrt(ax * ax + ay * ay + az * az);
        ax *= recipNorm;
        ay *= recipNorm;
        az *= recipNorm;

        // Estimated direction of gravity and vector perpendicular to magnetic
        // flux
        halfvx = q1 * q3 - q0 * q2;
        halfvy = q0 * q1 + q2 * q3;
        halfvz = q0 * q0 - 0.5f + q3 * q3;

        // Error is sum of cross product between estimated and measured
        // direction of gravity
        halfex = (ay * halfvz - az * halfvy);
        halfey = (az * halfvx - ax * halfvz);
        halfez = (ax * halfvy - ay * halfvx);

        // Compute and apply integral feedback if enabled
        if (twoKi > 0.0f) {
            integralFBx += twoKi * halfex *
                           (1.0f / sampleFreq);  // integral error scaled by Ki
            integralFBy += twoKi * halfey * (1.0f / sampleFreq);
            integralFBz += twoKi * halfez * (1.0f / sampleFreq);
            gx += integralFBx;  // apply integral feedback
            gy += integralFBy;
            gz += integralFBz;
        } else {
            integralFBx = 0.0f;  // prevent integral windup
            integralFBy = 0.0f;
            integralFBz = 0.0f;
        }

        // Apply proportional feedback
        gx += twoKp * halfex;
        gy += twoKp * halfey;
        gz += twoKp * halfez;
    }

    // Integrate rate of change of quaternion
    gx *= (0.5f * (1.0f / sampleFreq));  // pre-multiply common factors
    gy *= (0.5f * (1.0f / sampleFreq));
    gz *= (0.5f * (1.0f / sampleFreq));
    qa = q0;
    qb = q1;
    qc = q2;
    q0 += (-qb * gx - qc * gy - q3 * gz);
    q1 += (qa * gx + qc * gz - q3 * gy);
    q2 += (qa * gy - qb * gz + q3 * gx);
    q3 += (qa * gz + qb * gy - qc * gx);

    // Normalise quaternion
    recipNorm = invSqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    q0 *= recipNorm;
    q1 *= recipNorm;
    q2 *= recipNorm;
    q3 *= recipNorm;

    axis_val->pitch = asin(-2 * q1 * q3 + 2 * q0 * q2);  // pitch
    axis_val->roll  = atan2(2 * q2 * q3 + 2 * q0 * q1,
                   -2 * q1 * q1 - 2 * q2 * q2 + 1);  // roll
    axis_val->yaw   = atan2(2 * (q1 * q2 + q0 * q3),
                   q0 * q0 + q1 * q1 - q2 * q2 - q3 * q3);  // yaw

    axis_val->pitch *= RAD_TO_DEG;
    axis_val->yaw *= RAD_TO_DEG;
    // Declination of SparkFun Electronics (40°05'26.6"N 105°11'05.9"W) is
    //  8° 30' E  ± 0° 21' (or 8.5°) on 2016-07-19
    // - http://www.ngdc.noaa.gov/geomag-web/#declination
    axis_val->yaw -= 8.5;
    axis_val->roll *= RAD_TO_DEG;

}

typedef struct {
    float angle;      // The calculated angle
    float bias;       // The gyro bias
    float rate;       // The rate from gyro

    float P[2][2];    // Error covariance matrix
    float Q_angle;    // Process noise variance for angle
    float Q_bias;     // Process noise variance for bias
    float R_measure;  // Measurement noise variance
} KalmanFilter;

// Initialize Kalman Filter
static void KalmanFilter_Init(KalmanFilter *kf) 
{
    kf->angle = 0.0f;
    kf->bias = 0.0f;
    kf->rate = 0.0f;

    kf->P[0][0] = 0.0f;
    kf->P[0][1] = 0.0f;
    kf->P[1][0] = 0.0f;
    kf->P[1][1] = 0.0f;

    kf->Q_angle = 0.005f;
    kf->Q_bias = 0.003f;
    kf->R_measure = 0.03f;
}

// Update Kalman Filter
static float KalmanFilter_Update(KalmanFilter *kf, float newAngle, float newRate, float dt) 
{
    // Predict step
    kf->rate = newRate - kf->bias;
    kf->angle += dt * kf->rate;

    kf->P[0][0] += dt * (dt * kf->P[1][1] - kf->P[0][1] - kf->P[1][0] + kf->Q_angle);
    kf->P[0][1] -= dt * kf->P[1][1];
    kf->P[1][0] -= dt * kf->P[1][1];
    kf->P[1][1] += kf->Q_bias * dt;

    // Measurement update
    float S = kf->P[0][0] + kf->R_measure;
    float K[2]; // Kalman gain
    K[0] = kf->P[0][0] / S;
    K[1] = kf->P[1][0] / S;

    float y = newAngle - kf->angle; // Angle difference
    kf->angle += K[0] * y;
    kf->bias += K[1] * y;

    // Update error covariance matrix
    float P00_temp = kf->P[0][0];
    float P01_temp = kf->P[0][1];

    kf->P[0][0] -= K[0] * P00_temp;
    kf->P[0][1] -= K[0] * P01_temp;
    kf->P[1][0] -= K[1] * P00_temp;
    kf->P[1][1] -= K[1] * P01_temp;

    return kf->angle;
}

// Calculate Roll, Pitch, and Yaw
void calculateAttitude(float gyroX, float gyroY, float gyroZ, float accX, float accY, float accZ, float dt, bmi270_axis_t * axis_val)
{
    static KalmanFilter kfRoll, kfPitch;
    static int initialized = 0;

    if (!initialized) {
        KalmanFilter_Init(&kfRoll);
        KalmanFilter_Init(&kfPitch);
        initialized = 1;
    }

    // Calculate roll and pitch angles from accelerometer
    float accRoll = atan2f(accY, accZ) * 180.0f / M_PI;
    float accPitch = atan2f(-accX, sqrtf(accY * accY + accZ * accZ)) * 180.0f / M_PI;

    // Update Kalman filters for roll and pitch
    axis_val->roll = KalmanFilter_Update(&kfRoll, accRoll, gyroX, dt);
    axis_val->pitch = KalmanFilter_Update(&kfPitch, accPitch, gyroY, dt);

    // For yaw, integrate gyroZ directly (no accelerometer correction)
    static float yawAngle = 0.0f;
    yawAngle += gyroZ * dt;
    axis_val->yaw = yawAngle;
}