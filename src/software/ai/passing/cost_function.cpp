#include "software/ai/passing/cost_function.h"

#include <numeric>

#include "shared/parameter/cpp_dynamic_parameters.h"
#include "software/../shared/constants.h"
#include "software/ai/evaluation/calc_best_shot.h"
#include "software/ai/evaluation/pass.h"
#include "software/geom/algorithms/acute_angle.h"
#include "software/geom/algorithms/closest_point.h"
#include "software/geom/algorithms/contains.h"
#include "software/logger/logger.h"

double ratePass(const World& world, const Pass& pass, const Rectangle& zone,
                std::shared_ptr<const PassingConfig> passing_config)
{
    double static_pass_quality =
        getStaticPositionQuality(world.field(), pass.receiverPoint(), passing_config);

    double friendly_pass_rating =
        ratePassFriendlyCapability(world.friendlyTeam(), pass, passing_config);

    double enemy_pass_rating = ratePassEnemyRisk(world.enemyTeam(), pass, passing_config);

    double shoot_pass_rating =
        ratePassShootScore(world.field(), world.enemyTeam(), pass, passing_config);

    double in_region_quality = rectangleSigmoid(zone, pass.receiverPoint(), 0.2);

    // Place strict limits on the ball speed
    double min_pass_speed     = passing_config->getMinPassSpeedMPerS()->value();
    double max_pass_speed     = passing_config->getMaxPassSpeedMPerS()->value();
    double pass_speed_quality = sigmoid(pass.speed(), min_pass_speed, 0.2) *
                                (1 - sigmoid(pass.speed(), max_pass_speed, 0.2));

    return static_pass_quality * friendly_pass_rating * enemy_pass_rating *
           shoot_pass_rating * pass_speed_quality * in_region_quality;
}

double rateZone(const Field& field, const Team& enemy_team, const Rectangle& zone,
                const Point& ball_position,
                std::shared_ptr<const PassingConfig> passing_config)
{
    // TODO (#2021) improve and implement tests
    // Zones with their centers in bad positions are not good
    double static_pass_quality =
        getStaticPositionQuality(field, zone.centre(), passing_config);

    // Rate zones that are up the field higher to encourage progress up the field
    double pass_up_field_rating = zone.centre().x() / field.xLength();

    double enemy_risk_rating =
        (ratePassEnemyRisk(enemy_team,
                           Pass(ball_position, zone.negXNegYCorner(),
                                passing_config->getMaxPassSpeedMPerS()->value()),
                           passing_config) +
         ratePassEnemyRisk(enemy_team,
                           Pass(ball_position, zone.negXPosYCorner(),
                                passing_config->getMaxPassSpeedMPerS()->value()),
                           passing_config) +
         ratePassEnemyRisk(enemy_team,
                           Pass(ball_position, zone.posXNegYCorner(),
                                passing_config->getMaxPassSpeedMPerS()->value()),
                           passing_config) +
         ratePassEnemyRisk(enemy_team,
                           Pass(ball_position, zone.posXPosYCorner(),
                                passing_config->getMaxPassSpeedMPerS()->value()),
                           passing_config) +
         ratePassEnemyRisk(enemy_team,
                           Pass(ball_position, zone.centre(),
                                passing_config->getMaxPassSpeedMPerS()->value()),
                           passing_config)) /
        5.0;

    return pass_up_field_rating * static_pass_quality * enemy_risk_rating;
}

double ratePassShootScore(const Field& field, const Team& enemy_team, const Pass& pass,
                          std::shared_ptr<const PassingConfig> passing_config)
{
    double ideal_max_rotation_to_shoot_degrees =
        passing_config->getIdealMaxRotationToShootDegrees()->value();

    // Figure out the range of angles for which we have an open shot to the goal after
    // receiving the pass
    auto shot_opt = calcBestShotOnGoal(
        Segment(field.enemyGoalpostPos(), field.enemyGoalpostNeg()), pass.receiverPoint(),
        enemy_team.getAllRobots(), TeamType::ENEMY);

    Angle open_angle_to_goal = Angle::zero();
    Point shot_target        = field.enemyGoalCenter();
    if (shot_opt && shot_opt->getOpenAngle().abs() > Angle::fromDegrees(0))
    {
        open_angle_to_goal = shot_opt->getOpenAngle();
    }

    // Figure out what the maximum open angle of the goal could be from the receiver pos.
    Angle goal_angle = acuteAngle(field.enemyGoalpostNeg(), pass.receiverPoint(),
                                  field.enemyGoalpostPos())
                           .abs();
    double net_percent_open = 0;
    if (goal_angle > Angle::zero())
    {
        net_percent_open = open_angle_to_goal.toDegrees() / goal_angle.toDegrees();
    }

    // Create the shoot score by creating a sigmoid that goes to a large value as
    // the section of net we're shooting on approaches 100% (ie. completely open)
    double shot_openness_score = sigmoid(net_percent_open, 0.45, 0.95);

    // Prefer angles where the robot does not have to turn much after receiving the
    // pass to take the shot (or equivalently the shot deflection angle)
    //
    // Receiver robots on the friendly side, almost always, need to rotate a full 180
    // degrees to shoot on net. So we relax that requirement for both receiver and ball
    // locations on the friendly side
    //
    // TODO (#1987) This creates a very steep slope, find a better way to do this
    if (pass.receiverPoint().x() < 0 || pass.passerPoint().x() < 0)
    {
        ideal_max_rotation_to_shoot_degrees = 180;
    }
    Angle rotation_to_shot_target_after_pass = pass.receiverOrientation().minDiff(
        (shot_target - pass.receiverPoint()).orientation());
    double required_rotation_for_shot_score =
        1 - sigmoid(rotation_to_shot_target_after_pass.abs().toDegrees(),
                    ideal_max_rotation_to_shoot_degrees, 4);

    return shot_openness_score * required_rotation_for_shot_score;
}

double ratePassEnemyRisk(const Team& enemy_team, const Pass& pass,
                         std::shared_ptr<const PassingConfig> passing_config)
{
    double enemy_proximity_importance =
        passing_config->getEnemyProximityImportance()->value();

    // Calculate a risk score based on the distance of the enemy robots from the receive
    // point, based on an exponential function of the distance of each robot from the
    // receiver point
    auto enemy_robots                    = enemy_team.getAllRobots();
    double enemy_receiver_proximity_risk = 1;
    for (const Robot& enemy : enemy_team.getAllRobots())
    {
        double dist = (pass.receiverPoint() - enemy.position()).length();
        enemy_receiver_proximity_risk *=
            enemy_proximity_importance * std::exp(-dist * dist);
    }
    if (enemy_robots.empty())
    {
        enemy_receiver_proximity_risk = 0;
    }

    double intercept_risk = calculateInterceptRisk(enemy_team, pass, passing_config);

    // We want to rate a pass more highly if it is lower risk, so subtract from 1
    return 1 - std::max(intercept_risk, enemy_receiver_proximity_risk);
}

double calculateInterceptRisk(const Team& enemy_team, const Pass& pass,
                              std::shared_ptr<const PassingConfig> passing_config)
{
    // Return the highest risk for all the enemy robots, if there are any
    const std::vector<Robot>& enemy_robots = enemy_team.getAllRobots();
    if (enemy_robots.empty())
    {
        return 0;
    }
    std::vector<double> enemy_intercept_risks(enemy_robots.size());
    std::transform(
        enemy_robots.begin(), enemy_robots.end(), enemy_intercept_risks.begin(),
        [&](Robot robot) { return calculateInterceptRisk(robot, pass, passing_config); });
    return *std::max_element(enemy_intercept_risks.begin(), enemy_intercept_risks.end());
}

double calculateInterceptRisk(const Robot& enemy_robot, const Pass& pass,
                              std::shared_ptr<const PassingConfig> passing_config)
{
    // We estimate the intercept by the risk that the robot will get to the closest
    // point on the pass before the ball, and by the risk that the robot will get to
    // the reception point before the ball. We take the greater of these two risks.

    // If the enemy cannot intercept the pass at BOTH the closest point on the pass and
    // the receiver point for the pass, then it is guaranteed that it will not be
    // able to intercept the pass anywhere.

    // Figure out how long the enemy robot and ball will take to reach the closest
    // point on the pass to the enemy's current position
    Point closest_point_on_pass_to_robot = closestPoint(
        enemy_robot.position(), Segment(pass.passerPoint(), pass.receiverPoint()));
    Duration enemy_robot_time_to_closest_pass_point = getTimeToPositionForRobot(
        enemy_robot.position(), closest_point_on_pass_to_robot,
        ENEMY_ROBOT_MAX_SPEED_METERS_PER_SECOND,
        ENEMY_ROBOT_MAX_ACCELERATION_METERS_PER_SECOND_SQUARED, ROBOT_MAX_RADIUS_METERS);
    Duration ball_time_to_closest_pass_point = Duration::fromSeconds(
        (closest_point_on_pass_to_robot - pass.passerPoint()).length() / pass.speed());

    // Check for division by 0
    if (pass.speed() == 0)
    {
        ball_time_to_closest_pass_point =
            Duration::fromSeconds(std::numeric_limits<int>::max());
    }

    // Figure out how long the enemy robot and ball will take to reach the receive point
    // for the pass.
    Duration enemy_robot_time_to_pass_receive_position = getTimeToPositionForRobot(
        enemy_robot.position(), pass.receiverPoint(),
        ENEMY_ROBOT_MAX_SPEED_METERS_PER_SECOND,
        ENEMY_ROBOT_MAX_ACCELERATION_METERS_PER_SECOND_SQUARED, ROBOT_MAX_RADIUS_METERS);
    Duration ball_time_to_pass_receive_position = pass.estimatePassDuration();

    Duration enemy_reaction_time =
        Duration::fromSeconds(passing_config->getEnemyReactionTime()->value());

    double robot_ball_time_diff_at_closest_pass_point =
        ((enemy_robot_time_to_closest_pass_point + enemy_reaction_time) -
         (ball_time_to_closest_pass_point))
            .toSeconds();
    double robot_ball_time_diff_at_pass_receive_point =
        ((enemy_robot_time_to_pass_receive_position + enemy_reaction_time) -
         (ball_time_to_pass_receive_position))
            .toSeconds();

    double min_time_diff = std::min(robot_ball_time_diff_at_closest_pass_point,
                                    robot_ball_time_diff_at_pass_receive_point);

    // Whether or not the enemy will be able to intercept the pass can be determined
    // by whether or not they will be able to reach the pass receive position before
    // the pass does. As such, we place the time difference between the robot and ball
    // on a sigmoid that is centered at 0, and goes to 1 at positive values, 0 at
    // negative values.
    return 1 - sigmoid(min_time_diff, 0, 1);
}

double ratePassFriendlyCapability(Team friendly_team, const Pass& pass,
                                  std::shared_ptr<const PassingConfig> passing_config)
{
    // We need at least one robot to pass to
    if (friendly_team.getAllRobots().empty())
    {
        return 0;
    }

    // Special case where pass speed is 0
    if (pass.speed() == 0)
    {
        return 0;
    }

    // Get the robot that is closest to where the pass would be received
    Robot best_receiver = friendly_team.getAllRobots()[0];
    for (const Robot& robot : friendly_team.getAllRobots())
    {
        double distance = (robot.position() - pass.receiverPoint()).length();
        double curr_best_distance =
            (best_receiver.position() - pass.receiverPoint()).length();
        if (distance < curr_best_distance)
        {
            best_receiver = robot;
        }
    }

    // Figure out what time the robot would have to receive the ball at
    Duration ball_travel_time = Duration::fromSeconds(
        (pass.receiverPoint() - pass.passerPoint()).length() / pass.speed());
    Timestamp receive_time = best_receiver.timestamp() + ball_travel_time;

    // Figure out how long it would take our robot to get there
    Duration min_robot_travel_time = getTimeToPositionForRobot(
        best_receiver.position(), pass.receiverPoint(),
        best_receiver.robotConstants().robot_max_speed_m_per_s,
        best_receiver.robotConstants().robot_max_acceleration_m_per_s_2);
    Timestamp earliest_time_to_receive_point =
        best_receiver.timestamp() + min_robot_travel_time;

    // Figure out what angle the robot would have to be at to receive the ball
    Angle receive_angle = (pass.passerPoint() - best_receiver.position()).orientation();
    Duration time_to_receive_angle = getTimeToOrientationForRobot(
        best_receiver.orientation(), receive_angle,
        best_receiver.robotConstants().robot_max_ang_speed_rad_per_s,
        best_receiver.robotConstants().robot_max_ang_acceleration_rad_per_s_2);
    Timestamp earliest_time_to_receive_angle =
        best_receiver.timestamp() + time_to_receive_angle;

    // Figure out if rotation or moving will take us longer
    Timestamp latest_time_to_reciever_state =
        std::max(earliest_time_to_receive_angle, earliest_time_to_receive_point);

    // Create a sigmoid that goes to 0 as the time required to get to the reception
    // point exceeds the time we would need to get there by
    double sigmoid_width                  = 0.4;
    double time_to_receiver_state_slack_s = 0.25;

    return sigmoid(
        receive_time.toSeconds(),
        latest_time_to_reciever_state.toSeconds() + time_to_receiver_state_slack_s,
        sigmoid_width);
}

double getStaticPositionQuality(const Field& field, const Point& position,
                                std::shared_ptr<const PassingConfig> passing_config)
{
    // This constant is used to determine how steep the sigmoid slopes below are
    static const double sig_width = 0.1;

    // The offset from the sides of the field for the center of the sigmoid functions
    double x_offset = passing_config->getStaticFieldPositionQualityXOffset()->value();
    double y_offset = passing_config->getStaticFieldPositionQualityYOffset()->value();
    double friendly_goal_weight =
        passing_config->getStaticFieldPositionQualityFriendlyGoalDistanceWeight()
            ->value();

    // Make a slightly smaller field, and positive weight values in this reduced field
    double half_field_length = field.xLength() / 2;
    double half_field_width  = field.yLength() / 2;
    Rectangle reduced_size_field(
        Point(-half_field_length + x_offset, -half_field_width + y_offset),
        Point(half_field_length - x_offset, half_field_width - y_offset));
    double on_field_quality = rectangleSigmoid(reduced_size_field, position, sig_width);

    // Add a negative weight for positions closer to our goal
    Vector vec_to_friendly_goal = Vector(field.friendlyGoalCenter().x() - position.x(),
                                         field.friendlyGoalCenter().y() - position.y());
    double distance_to_friendly_goal = vec_to_friendly_goal.length();
    double near_friendly_goal_quality =
        (1 -
         std::exp(-friendly_goal_weight * (std::pow(5, -2 + distance_to_friendly_goal))));

    // Add a strong negative weight for positions within the enemy defense area, as we
    // cannot pass there
    double in_enemy_defense_area_quality =
        1 - rectangleSigmoid(field.enemyDefenseArea(), position, sig_width);

    return on_field_quality * near_friendly_goal_quality * in_enemy_defense_area_quality;
}
