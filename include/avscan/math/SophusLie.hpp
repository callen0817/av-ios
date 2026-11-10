#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace avscan {
namespace math {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;
constexpr double kEps = 1e-10;

struct Vec2d {
    double x{0.0}, y{0.0};
    constexpr Vec2d() = default;
    constexpr Vec2d(double x_, double y_) : x(x_), y(y_) {}
    double norm() const { return std::sqrt(x * x + y * y); }
};

struct Vec3d {
    double x{0.0}, y{0.0}, z{0.0};
    constexpr Vec3d() = default;
    constexpr Vec3d(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    double& operator[](size_t i) { return (&x)[i]; }
    const double& operator[](size_t i) const { return (&x)[i]; }

    Vec3d operator+(const Vec3d& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3d operator-(const Vec3d& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3d operator-() const { return {-x, -y, -z}; }
    Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3d operator/(double s) const { return {x / s, y / s, z / s}; }

    Vec3d& operator+=(const Vec3d& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3d& operator-=(const Vec3d& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3d& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }

    double dot(const Vec3d& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3d cross(const Vec3d& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double squaredNorm() const { return x * x + y * y + z * z; }
    double norm() const { return std::sqrt(squaredNorm()); }
    Vec3d normalized() const {
        double n = norm();
        return (n > 1e-12) ? (*this / n) : Vec3d(0.0, 0.0, 0.0);
    }
};

inline Vec3d operator*(double s, const Vec3d& v) { return v * s; }

struct Mat3d {
    double m[3][3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    static Mat3d Identity() {
        Mat3d r;
        r.m[0][0] = 1; r.m[0][1] = 0; r.m[0][2] = 0;
        r.m[1][0] = 0; r.m[1][1] = 1; r.m[1][2] = 0;
        r.m[2][0] = 0; r.m[2][1] = 0; r.m[2][2] = 1;
        return r;
    }

    static Mat3d Zero() {
        Mat3d r;
        std::memset(r.m, 0, sizeof(r.m));
        return r;
    }

    static Mat3d OuterProduct(const Vec3d& a, const Vec3d& b) {
        Mat3d r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = a[i] * b[j];
        return r;
    }

    double& operator()(int r, int c) { return m[r][c]; }
    const double& operator()(int r, int c) const { return m[r][c]; }

    Mat3d operator+(const Mat3d& o) const {
        Mat3d r = Zero();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][j] + o.m[i][j];
        return r;
    }

    Mat3d operator-(const Mat3d& o) const {
        Mat3d r = Zero();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][j] - o.m[i][j];
        return r;
    }

    Mat3d operator*(double s) const {
        Mat3d r = Zero();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][j] * s;
        return r;
    }

    Vec3d operator*(const Vec3d& v) const {
        return {
            m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z
        };
    }

    Mat3d operator*(const Mat3d& o) const {
        Mat3d r = Zero();
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j)
                    r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }

    Mat3d transpose() const {
        Mat3d r = Zero();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[j][i];
        return r;
    }

    double trace() const { return m[0][0] + m[1][1] + m[2][2]; }

    double determinant() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }

    Mat3d inverse() const {
        double det = determinant();
        if (std::abs(det) < 1e-14) {
            return Identity();
        }
        double invDet = 1.0 / det;
        Mat3d r;
        r.m[0][0] =  (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * invDet;
        r.m[0][1] = -(m[0][1] * m[2][2] - m[0][2] * m[2][1]) * invDet;
        r.m[0][2] =  (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * invDet;
        r.m[1][0] = -(m[1][0] * m[2][2] - m[1][2] * m[2][0]) * invDet;
        r.m[1][1] =  (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * invDet;
        r.m[1][2] = -(m[0][0] * m[1][2] - m[0][2] * m[1][0]) * invDet;
        r.m[2][0] =  (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * invDet;
        r.m[2][1] = -(m[0][0] * m[2][1] - m[0][1] * m[2][0]) * invDet;
        r.m[2][2] =  (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * invDet;
        return r;
    }

    double frobeniusNorm() const {
        double s = 0.0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                s += m[i][j] * m[i][j];
        return std::sqrt(s);
    }
};

struct Quatd {
    double x{0.0}, y{0.0}, z{0.0}, w{1.0};
    constexpr Quatd() = default;
    constexpr Quatd(double x_, double y_, double z_, double w_) : x(x_), y(y_), z(z_), w(w_) {}

    void normalize() {
        double n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n > 1e-12) {
            x /= n; y /= n; z /= n; w /= n;
        } else {
            x = 0.0; y = 0.0; z = 0.0; w = 1.0;
        }
    }

    Quatd normalized() const {
        Quatd q = *this;
        q.normalize();
        return q;
    }

    Quatd conjugate() const { return {-x, -y, -z, w}; }

    Mat3d toRotationMatrix() const {
        Quatd q = normalized();
        Mat3d R;
        double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

        R(0, 0) = 1.0 - 2.0 * (yy + zz);
        R(0, 1) = 2.0 * (xy - wz);
        R(0, 2) = 2.0 * (xz + wy);
        R(1, 0) = 2.0 * (xy + wz);
        R(1, 1) = 1.0 - 2.0 * (xx + zz);
        R(1, 2) = 2.0 * (yz - wx);
        R(2, 0) = 2.0 * (xz - wy);
        R(2, 1) = 2.0 * (yz + wx);
        R(2, 2) = 1.0 - 2.0 * (xx + yy);
        return R;
    }

    static Quatd fromRotationMatrix(const Mat3d& R) {
        double tr = R.trace();
        Quatd q;
        if (tr > 0.0) {
            double s = std::sqrt(tr + 1.0) * 2.0;
            q.w = 0.25 * s;
            q.x = (R(2, 1) - R(1, 2)) / s;
            q.y = (R(0, 2) - R(2, 0)) / s;
            q.z = (R(1, 0) - R(0, 1)) / s;
        } else if ((R(0, 0) > R(1, 1)) && (R(0, 0) > R(2, 2))) {
            double s = std::sqrt(1.0 + R(0, 0) - R(1, 1) - R(2, 2)) * 2.0;
            q.w = (R(2, 1) - R(1, 2)) / s;
            q.x = 0.25 * s;
            q.y = (R(0, 1) + R(1, 0)) / s;
            q.z = (R(0, 2) + R(2, 0)) / s;
        } else if (R(1, 1) > R(2, 2)) {
            double s = std::sqrt(1.0 + R(1, 1) - R(0, 0) - R(2, 2)) * 2.0;
            q.w = (R(0, 2) - R(2, 0)) / s;
            q.x = (R(0, 1) + R(1, 0)) / s;
            q.y = 0.25 * s;
            q.z = (R(1, 2) + R(2, 1)) / s;
        } else {
            double s = std::sqrt(1.0 + R(2, 2) - R(0, 0) - R(1, 1)) * 2.0;
            q.w = (R(1, 0) - R(0, 1)) / s;
            q.x = (R(0, 2) + R(2, 0)) / s;
            q.y = (R(1, 2) + R(2, 1)) / s;
            q.z = 0.25 * s;
        }
        q.normalize();
        return q;
    }

    Vec3d toEulerRollPitchYaw() const {
        Quatd q = normalized();
        double roll = std::atan2(2.0 * (q.w * q.x + q.y * q.z),
                                 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
        double sinp = std::clamp(2.0 * (q.w * q.y - q.z * q.x), -1.0, 1.0);
        double pitch = std::asin(sinp);
        double yaw = std::atan2(2.0 * (q.w * q.z + q.x * q.y),
                                1.0 - 2.0 * (q.y * q.y + q.z * q.z));
        return {roll, pitch, yaw};
    }
};

// Lie group SO(3) implementation (Sophus-compatible)
struct SO3d {
    Mat3d R{Mat3d::Identity()};

    SO3d() = default;
    explicit SO3d(const Mat3d& R_) : R(R_) {}
    explicit SO3d(const Quatd& q) : R(q.toRotationMatrix()) {}

    static Mat3d hat(const Vec3d& w) {
        Mat3d W = Mat3d::Zero();
        W(0, 1) = -w.z; W(0, 2) =  w.y;
        W(1, 0) =  w.z; W(1, 2) = -w.x;
        W(2, 0) = -w.y; W(2, 1) =  w.x;
        return W;
    }

    static Vec3d vee(const Mat3d& W) {
        return {
            0.5 * (W(2, 1) - W(1, 2)),
            0.5 * (W(0, 2) - W(2, 0)),
            0.5 * (W(1, 0) - W(0, 1))
        };
    }

    static SO3d exp(const Vec3d& omega) {
        double theta = omega.norm();
        Mat3d K = hat(omega);
        if (theta < 1e-8) {
            return SO3d(Mat3d::Identity() + K + (K * K) * 0.5);
        }
        double s = std::sin(theta) / theta;
        double c = (1.0 - std::cos(theta)) / (theta * theta);
        return SO3d(Mat3d::Identity() + K * s + (K * K) * c);
    }

    Vec3d log() const {
        double cos_theta = std::clamp(0.5 * (R.trace() - 1.0), -1.0, 1.0);
        double theta = std::acos(cos_theta);
        if (theta < 1e-8) {
            return vee(R - Mat3d::Identity());
        }
        if (std::abs(kPi - theta) < 1e-5) {
            Quatd q = Quatd::fromRotationMatrix(R);
            Vec3d axis(q.x, q.y, q.z);
            return axis.normalized() * theta;
        }
        return vee(R - R.transpose()) * (theta / (2.0 * std::sin(theta)));
    }

    static Mat3d rightJacobian(const Vec3d& omega) {
        double theta = omega.norm();
        Mat3d K = hat(omega);
        if (theta < 1e-8) {
            return Mat3d::Identity() - K * 0.5 + (K * K) * (1.0 / 6.0);
        }
        double theta2 = theta * theta;
        double theta3 = theta2 * theta;
        double c1 = (1.0 - std::cos(theta)) / theta2;
        double c2 = (theta - std::sin(theta)) / theta3;
        return Mat3d::Identity() - K * c1 + (K * K) * c2;
    }

    static Mat3d rightJacobianInverse(const Vec3d& omega) {
        double theta = omega.norm();
        Mat3d K = hat(omega);
        if (theta < 1e-8) {
            return Mat3d::Identity() + K * 0.5 + (K * K) * (1.0 / 12.0);
        }
        double half_theta = 0.5 * theta;
        double cot_half = std::cos(half_theta) / std::sin(half_theta);
        double c2 = (1.0 / (theta * theta)) * (1.0 - half_theta * cot_half);
        return Mat3d::Identity() + K * 0.5 + (K * K) * c2;
    }

    SO3d inverse() const { return SO3d(R.transpose()); }
    SO3d operator*(const SO3d& o) const { return SO3d(R * o.R); }
    Vec3d operator*(const Vec3d& p) const { return R * p; }
    Quatd unit_quaternion() const { return Quatd::fromRotationMatrix(R); }
};

using Vec6d = std::array<double, 6>;

// Lie group SE(3) implementation (Sophus-compatible: [translation(3), rotation(3)])
struct SE3d {
    SO3d so3{};
    Vec3d t{0.0, 0.0, 0.0};

    SE3d() = default;
    SE3d(const SO3d& r_, const Vec3d& t_) : so3(r_), t(t_) {}
    SE3d(const Mat3d& R_, const Vec3d& t_) : so3(R_), t(t_) {}
    SE3d(const Quatd& q_, const Vec3d& t_) : so3(q_), t(t_) {}

    static SE3d Identity() { return SE3d(); }

    const Mat3d& rotationMatrix() const { return so3.R; }
    const Vec3d& translation() const { return t; }

    Vec3d transform(const Vec3d& p) const { return so3.R * p + t; }
    Vec3d operator*(const Vec3d& p) const { return transform(p); }

    SE3d operator*(const SE3d& o) const {
        return SE3d(so3 * o.so3, so3.R * o.t + t);
    }

    SE3d inverse() const {
        Mat3d R_inv = so3.R.transpose();
        return SE3d(R_inv, -(R_inv * t));
    }

    static SE3d exp(const Vec6d& xi) {
        Vec3d rho(xi[0], xi[1], xi[2]);
        Vec3d phi(xi[3], xi[4], xi[5]);
        SO3d rot = SO3d::exp(phi);
        Mat3d V = SO3d::rightJacobian(-phi);
        return SE3d(rot, V * rho);
    }

    Vec6d log() const {
        Vec3d phi = so3.log();
        Mat3d V_inv = SO3d::rightJacobianInverse(-phi);
        Vec3d rho = V_inv * t;
        return {rho.x, rho.y, rho.z, phi.x, phi.y, phi.z};
    }

    static SE3d interpolate(const SE3d& T0, const SE3d& T1, double alpha) {
        double a = std::clamp(alpha, 0.0, 1.0);
        SE3d dT = T0.inverse() * T1;
        Vec6d xi = dT.log();
        for (double& v : xi) v *= a;
        return T0 * SE3d::exp(xi);
    }
};

// Closed-form Jacobi Eigensolver for 3x3 real symmetric matrices
// Returns eigenvalues sorted ascending (evals[0] <= evals[1] <= evals[2])
// and corresponding orthonormal eigenvectors as columns of evecs.
struct SymEigen3d {
    Vec3d eigenvalues{0.0, 0.0, 0.0};
    Mat3d eigenvectors{Mat3d::Identity()};

    static SymEigen3d compute(const Mat3d& A_in, int maxSweeps = 30) {
        Mat3d A = A_in;
        Mat3d V = Mat3d::Identity();

        for (int sweep = 0; sweep < maxSweeps; ++sweep) {
            double off = std::abs(A(0, 1)) + std::abs(A(0, 2)) + std::abs(A(1, 2));
            if (off < 1e-14) break;

            const int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
            for (int k = 0; k < 3; ++k) {
                int p = pairs[k][0];
                int q = pairs[k][1];
                double apq = A(p, q);
                if (std::abs(apq) < 1e-15) continue;

                double app = A(p, p);
                double aqq = A(q, q);
                double tau = (aqq - app) / (2.0 * apq);
                double t = (tau >= 0.0)
                    ? (1.0 / (tau + std::sqrt(1.0 + tau * tau)))
                    : (-1.0 / (-tau + std::sqrt(1.0 + tau * tau)));
                double c = 1.0 / std::sqrt(1.0 + t * t);
                double s = t * c;

                A(p, p) = app - t * apq;
                A(q, q) = aqq + t * apq;
                A(p, q) = 0.0;
                A(q, p) = 0.0;

                for (int r = 0; r < 3; ++r) {
                    if (r != p && r != q) {
                        double arp = A(r, p);
                        double arq = A(r, q);
                        A(r, p) = c * arp - s * arq;
                        A(p, r) = A(r, p);
                        A(r, q) = s * arp + c * arq;
                        A(q, r) = A(r, q);
                    }
                    double vrp = V(r, p);
                    double vrq = V(r, q);
                    V(r, p) = c * vrp - s * vrq;
                    V(r, q) = s * vrp + c * vrq;
                }
            }
        }

        Vec3d evals(A(0, 0), A(1, 1), A(2, 2));
        int idx[3] = {0, 1, 2};
        if (evals[idx[0]] > evals[idx[1]]) std::swap(idx[0], idx[1]);
        if (evals[idx[1]] > evals[idx[2]]) std::swap(idx[1], idx[2]);
        if (evals[idx[0]] > evals[idx[1]]) std::swap(idx[0], idx[1]);

        SymEigen3d res;
        for (int c = 0; c < 3; ++c) {
            res.eigenvalues[c] = evals[idx[c]];
            for (int r = 0; r < 3; ++r) {
                res.eigenvectors(r, c) = V(r, idx[c]);
            }
        }
        // Ensure right-handed eigenvector basis
        Vec3d c0(res.eigenvectors(0, 0), res.eigenvectors(1, 0), res.eigenvectors(2, 0));
        Vec3d c1(res.eigenvectors(0, 1), res.eigenvectors(1, 1), res.eigenvectors(2, 1));
        Vec3d c2(res.eigenvectors(0, 2), res.eigenvectors(1, 2), res.eigenvectors(2, 2));
        if (c0.cross(c1).dot(c2) < 0.0) {
            for (int r = 0; r < 3; ++r) res.eigenvectors(r, 2) = -res.eigenvectors(r, 2);
        }
        return res;
    }
};

// Orthogonal Procrustes / SVD Rotation Projection for 3x3 Matrices
inline Mat3d projectToSO3(const Mat3d& M) {
    Mat3d MtM = M.transpose() * M;
    SymEigen3d eig = SymEigen3d::compute(MtM);
    Mat3d U = Mat3d::Zero();
    Mat3d V = eig.eigenvectors;
    for (int j = 0; j < 3; ++j) {
        double sigma = std::sqrt(std::max(eig.eigenvalues[j], 1e-14));
        Vec3d vj(V(0, j), V(1, j), V(2, j));
        Vec3d uj = (M * vj) / sigma;
        for (int i = 0; i < 3; ++i) U(i, j) = uj[i];
    }
    // Re-orthonormalize U columns
    Vec3d u2(U(0, 2), U(1, 2), U(2, 2));
    u2 = u2.normalized();
    Vec3d u1(U(0, 1), U(1, 1), U(2, 1));
    u1 = (u1 - u2 * u1.dot(u2)).normalized();
    Vec3d u0 = u1.cross(u2).normalized();
    for (int i = 0; i < 3; ++i) {
        U(i, 0) = u0[i];
        U(i, 1) = u1[i];
        U(i, 2) = u2[i];
    }
    Mat3d R = U * V.transpose();
    if (R.determinant() < 0.0) {
        for (int i = 0; i < 3; ++i) U(i, 0) = -U(i, 0);
        R = U * V.transpose();
    }
    return R;
}

}  // namespace math
}  // namespace avscan
