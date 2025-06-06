#include "musicat/audio_config.h"
#include "musicat/audio_processing.h"
#include "musicat/child.h"
#include "musicat/child/command.h"
#include "musicat/config.h"
#include "musicat/db.h"
#include "musicat/mctrack.h"
#include "musicat/musicat.h"
#include "musicat/player.h"
#include <memory>
#include <string>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace musicat::player
{
namespace cc = child::command;
namespace cw = child::worker;

static effect_states_list_t effect_states_list = {};
std::mutex effect_states_list_m; // EXTERN_VARIABLE

class EffectStatesListing
{
    const dpp::snowflake guild_id;
    bool already_exist;

  public:
    EffectStatesListing (
        const dpp::snowflake &guild_id,
        handle_effect_chain_change_states_t *effect_states_ptr)
        : guild_id (guild_id), already_exist (false)
    {
        std::lock_guard lk (effect_states_list_m);

        auto i = effect_states_list.begin ();
        while (i != effect_states_list.end ())
            {
                if ((*i)->guild_player->guild_id != guild_id)
                    {
                        i++;
                        continue;
                    }

                already_exist = true;
                break;
            }

        if (!already_exist)
            {
                effect_states_list.push_back (effect_states_ptr);
            }

        else
            std::cerr << "[musicat::player::EffectStatesListing ERROR] "
                         "Effect States already exist: "
                      << guild_id << '\n';
    }

    ~EffectStatesListing ()
    {
        if (already_exist)
            return;

        std::lock_guard lk (effect_states_list_m);

        auto i = effect_states_list.begin ();
        while (i != effect_states_list.end ())
            {
                if ((*i)->guild_player->guild_id != guild_id)
                    {
                        i++;
                        continue;
                    }

                i = effect_states_list.erase (i);
            }
    }
};

int
wait_for_ready_event (const dpp::snowflake &guild_id)
{
    auto player_manager = get_player_manager_ptr ();
    if (!player_manager)
        return 1;

    if (player_manager->wait_for_vc_ready (guild_id) == 0)
        {
            auto guild_player = player_manager->get_player (guild_id);

            // this should never be happening
            if (!guild_player)
                return 0;

            // reset current byte
            guild_player->reset_first_track_current_byte ();

            if (auto *vc = guild_player->get_voice_client (); vc != nullptr)
                {
                    // check stage channel routine
                    player_manager->prepare_play_stage_channel_routine (
                        vc, dpp::find_guild (guild_id));
                }
        }

    return 0;
}

std::string
get_ffmpeg_vibrato_args (bool has_f, bool has_d,
                         std::shared_ptr<Player> &guild_player)
{
    std::string v_args;

    if (has_f)
        {
            v_args += "f=" + std::to_string (guild_player->vibrato_f);
        }

    if (has_d)
        {
            if (has_f)
                v_args += ':';

            int64_t nd = guild_player->vibrato_d;
            v_args += "d=" + std::to_string (nd > 0 ? (float)nd / 100 : nd);
        }

    return v_args;
}

std::string
get_ffmpeg_tremolo_args (bool has_f, bool has_d,
                         std::shared_ptr<Player> &guild_player)
{
    std::string v_args;

    if (has_f)
        {
            v_args += "f=" + std::to_string (guild_player->tremolo_f);
        }

    if (has_d)
        {
            if (has_f)
                v_args += ':';

            int64_t nd = guild_player->tremolo_d;
            v_args += "d=" + std::to_string (nd > 0 ? (float)nd / 100 : nd);
        }

    return v_args;
}

std::string
get_ffmpeg_pitch_args (int pitch)
{
    if (pitch == 0)
        return "";

    constexpr int64_t samp_per_percent = 24000 / 100;
    constexpr double tempo_per_percent = 0.5 / 100;

    int64_t sample = 48000 + (pitch * (-samp_per_percent));
    double tempo = 1.0 + ((double)pitch * (-tempo_per_percent));

    /*
        100=24000,0.5=-24000,-0.5=48000+(100*(-(24000/100))),1.0+(100*(-(0.5/100)))
        0=48000,1.0
        -100=72000,1.5=+24000,+0.5=48000+(-100*(-(24000/100))),1.0+(-100*(-(0.5/100)))
        -200=96000,2.0=+48000,+1.0=48000+(-200*(-(24000/100))),1.0+(-200*(-(0.5/100)))
        -300=120000,2.5=+72000,+1.5
        -400=144000,3.0=+96000,+2.0
    */

    // exclusive effect
    return "!aresample=" + std::to_string (sample)
           + ",atempo=" + std::to_string (tempo);
}

handle_effect_chain_change_states_t *
get_effect_states (const dpp::snowflake &guild_id)
{
    auto i = effect_states_list.begin ();
    while (i != effect_states_list.end ())
        {
            if ((*i)->guild_player->guild_id != guild_id)
                {
                    i++;
                    continue;
                }

            return *i;
        }

    return nullptr;
}

effect_states_list_t *
get_effect_states_list ()
{
    return &effect_states_list;
}

void
handle_effect_chain_change (handle_effect_chain_change_states_t &states)
{
    const std::string dbg_str_arg = cc::get_dbg_str_arg ();

    auto *vc = states.guild_player->get_voice_client ();
    const bool has_vc = vc != nullptr;

    bool track_seek_queried = !states.track.seek_to.empty ();
    if (track_seek_queried)
        {
            std::string cmd
                = cc::command_options_keys_t.command + '='
                  + cc::command_options_keys_t.seek + ';'
                  + cc::command_options_keys_t.seek + '='
                  + cc::sanitize_command_value (states.track.seek_to) + ';'
                  + dbg_str_arg;

            cc::write_command (cmd, states.command_fd, "Manager::stream");

            // clear voice_client audio buffer
            if (has_vc)
                vc->stop_audio ();

            states.track.seek_to = "";

            // drain current buffer while waiting for notification

            struct pollfd datapfds[1];
            datapfds[0].events = POLLIN;
            datapfds[0].fd = states.read_fd;

            // struct pollfd notifpfds[1];
            // notifpfds[0].events = POLLIN;
            // notifpfds[0].fd = states.notification_fd;

            // notification buffer
            // char nbuf[CMD_BUFSIZE + 1];
            // drain buffer
            uint8_t buffer[4];
            short revents = 0;

            while (true)
                {
                    bool has_data
                        = (poll (datapfds, 1, 100) > 0)
                          && ((revents = datapfds[0].revents) & POLLIN);

                    if (revents & POLLHUP || revents & POLLERR
                        || revents & POLLNVAL)
                        {
                            std::cerr
                                << "[Manager::stream ERROR] POLL SEEK: gid("
                                << (has_vc ? vc->server_id.str () : "-1")
                                << ") cid("
                                << (has_vc ? vc->channel_id.str () : "-1")
                                << ")\n";

                            break;
                        }

                    if (has_data)
                        {
                            // read 4 by 4 as stereo pcm frame size says so
                            ssize_t rsiz = read (states.read_fd, buffer, 4);

                            // this is the notification message to stop reading
                            // i write whatever i want!
                            if (rsiz == 4 && buffer[0] == 'B'
                                && buffer[1] == 'O' && buffer[2] == 'O'
                                && buffer[3] == 'B')
                                break;
                        }

                    // bool has_notification = (poll (notifpfds, 1, 0) > 0)
                    //                         && (notifpfds[0].revents &
                    //                         POLLIN);
                    // if (has_notification)
                    //     {
                    //         size_t nread_size = read
                    //         (states.notification_fd,
                    //                                   nbuf, CMD_BUFSIZE);

                    //         if (nread_size > 0)
                    //             // just ignore whatever message it has
                    //             // one job, simply break when notified
                    //             break;
                    //     }
                }
        }

    bool volume_queried = states.guild_player->set_volume != -1;
    if (volume_queried)
        {
            std::string cmd
                = cc::command_options_keys_t.command + '='
                  + cc::command_options_keys_t.volume + ';'
                  + cc::command_options_keys_t.volume + '='
                  + std::to_string (states.guild_player->set_volume) + ';'
                  + dbg_str_arg;

            cc::write_command (cmd, states.command_fd, "Manager::stream");

            states.guild_player->volume = states.guild_player->set_volume;
            states.guild_player->set_volume = -1;
        }

    bool should_write_helper_chain_cmd = false;
    std::string helper_chain_cmd = cc::command_options_keys_t.command + '='
                                   + cc::command_options_keys_t.helper_chain
                                   + ';';

    bool tempo_queried = states.guild_player->set_tempo;
    if (tempo_queried)
        {
            std::string new_fx
                = states.guild_player->tempo == 1.0
                      ? ""
                      : "!atempo="
                            + std::to_string (states.guild_player->tempo);

            std::string cmd = cc::command_options_keys_t.helper_chain + '='
                              + cc::sanitize_command_value (new_fx) + ';';

            helper_chain_cmd += cmd;

            states.guild_player->set_tempo = false;
            should_write_helper_chain_cmd = true;
        }

    bool pitch_queried = states.guild_player->set_pitch;
    if (pitch_queried)
        {
            std::string new_fx
                = get_ffmpeg_pitch_args (states.guild_player->pitch);

            std::string cmd = cc::command_options_keys_t.helper_chain + '='
                              + cc::sanitize_command_value (new_fx) + ';';

            helper_chain_cmd += cmd;

            states.guild_player->set_pitch = false;
            should_write_helper_chain_cmd = true;
        }

    bool equalizer_queried = states.guild_player->set_equalizer;
    if (equalizer_queried)
        {
            std::string new_equalizer
                = states.guild_player->equalizer == "0"
                      ? ""
                      : "superequalizer=" + states.guild_player->equalizer;

            std::string cmd = cc::command_options_keys_t.helper_chain + '='
                              + cc::sanitize_command_value (new_equalizer)
                              + ';';

            helper_chain_cmd += cmd;

            if (new_equalizer.empty ())
                states.guild_player->equalizer.clear ();

            states.guild_player->set_equalizer = false;
            should_write_helper_chain_cmd = true;
        }

    bool resample_queried = states.guild_player->set_sampling_rate;
    if (resample_queried)
        {
            std::string new_resample
                = states.guild_player->sampling_rate == -1
                      ? ""
                      : "aresample="
                            + std::to_string (
                                states.guild_player->sampling_rate);

            std::string cmd = cc::command_options_keys_t.helper_chain + '='
                              + cc::sanitize_command_value (new_resample)
                              + ';';

            helper_chain_cmd += cmd;

            states.guild_player->set_sampling_rate = false;
            should_write_helper_chain_cmd = true;
        }

    bool vibrato_queried = states.guild_player->set_vibrato;
    bool has_vibrato_f, has_vibrato_d;

    has_vibrato_f = states.guild_player->vibrato_f != -1;
    has_vibrato_d = states.guild_player->vibrato_d != -1;

    if (vibrato_queried)
        {

            std::string new_vibrato
                = (!has_vibrato_f && !has_vibrato_d)
                      ? ""
                      : "vibrato="
                            + get_ffmpeg_vibrato_args (has_vibrato_f,
                                                       has_vibrato_d,
                                                       states.guild_player);

            std::string cmd = cc::command_options_keys_t.helper_chain + '='
                              + cc::sanitize_command_value (new_vibrato) + ';';

            helper_chain_cmd += cmd;

            states.guild_player->set_vibrato = false;
            should_write_helper_chain_cmd = true;
        }

    bool tremolo_queried = states.guild_player->set_tremolo;
    bool has_tremolo_f, has_tremolo_d;

    has_tremolo_f = states.guild_player->tremolo_f != -1;
    has_tremolo_d = states.guild_player->tremolo_d != -1;

    if (tremolo_queried)
        {

            std::string new_tremolo
                = (!has_tremolo_f && !has_tremolo_d)
                      ? ""
                      : "tremolo="
                            + get_ffmpeg_tremolo_args (has_tremolo_f,
                                                       has_tremolo_d,
                                                       states.guild_player);

            std::string cmd = cc::command_options_keys_t.helper_chain + '='
                              + cc::sanitize_command_value (new_tremolo) + ';';

            helper_chain_cmd += cmd;

            states.guild_player->set_tremolo = false;
            should_write_helper_chain_cmd = true;
        }

    bool earwax_queried = states.guild_player->set_earwax;

    if (earwax_queried)
        {
            std::string new_fx
                = states.guild_player->earwax == false ? "" : "earwax";

            std::string cmd = cc::command_options_keys_t.helper_chain + '='
                              + cc::sanitize_command_value (new_fx) + ';';

            helper_chain_cmd += cmd;

            states.guild_player->set_earwax = false;

            should_write_helper_chain_cmd = true;
        }

    if (!should_write_helper_chain_cmd)
        return;

    // update fx_states in db
    database::update_guild_player_config (
        states.guild_player->guild_id, NULL, NULL, NULL,
        states.guild_player->fx_states_to_json ());

    // check for existed non queried and add it to cmd

    if (!tempo_queried && states.guild_player->tempo != 1.0)
        {
            std::string cmd
                = cc::command_options_keys_t.helper_chain + '='
                  + cc::sanitize_command_value (
                      "!atempo=" + std::to_string (states.guild_player->tempo))
                  + ';';

            helper_chain_cmd += cmd;
        }

    if (!pitch_queried && states.guild_player->pitch != 0)
        {
            std::string cmd
                = cc::command_options_keys_t.helper_chain + '='
                  + cc::sanitize_command_value (
                      get_ffmpeg_pitch_args (states.guild_player->pitch))
                  + ';';

            helper_chain_cmd += cmd;
        }

    if (!equalizer_queried && !states.guild_player->equalizer.empty ())
        {
            std::string cmd
                = cc::command_options_keys_t.helper_chain + '='
                  + cc::sanitize_command_value (
                      "superequalizer=" + states.guild_player->equalizer)
                  + ';';

            helper_chain_cmd += cmd;
        }

    if (!resample_queried && states.guild_player->sampling_rate != -1)
        {
            std::string cmd
                = cc::command_options_keys_t.helper_chain + '='
                  + cc::sanitize_command_value (
                      "aresample="
                      + std::to_string (states.guild_player->sampling_rate))
                  + ';';

            helper_chain_cmd += cmd;
        }

    if (!vibrato_queried && (has_vibrato_f || has_vibrato_d))
        {
            std::string v_args = get_ffmpeg_vibrato_args (
                has_vibrato_f, has_vibrato_d, states.guild_player);

            std::string cmd
                = cc::command_options_keys_t.helper_chain + '='
                  + cc::sanitize_command_value ("vibrato=" + v_args) + ';';

            helper_chain_cmd += cmd;
        }

    if (!tremolo_queried && (has_tremolo_f || has_tremolo_d))
        {
            std::string v_args = get_ffmpeg_tremolo_args (
                has_tremolo_f, has_tremolo_d, states.guild_player);

            std::string cmd
                = cc::command_options_keys_t.helper_chain + '='
                  + cc::sanitize_command_value ("tremolo=" + v_args) + ';';

            helper_chain_cmd += cmd;
        }

    if (!earwax_queried && states.guild_player->earwax)
        {
            std::string cmd = cc::command_options_keys_t.helper_chain + '='
                              + cc::sanitize_command_value ("earwax") + ';';

            helper_chain_cmd += cmd;
        }

    cc::write_command (helper_chain_cmd + dbg_str_arg, states.command_fd,
                       "Manager::stream");
}

constexpr const char *msprrfmt
    = "[Manager::stream ERROR] Processor not ready or exited: %s\n";

void
Manager::stream (const dpp::snowflake &guild_id, player::MCTrack &track)
{
    auto guild_player = guild_id ? this->get_player (guild_id) : nullptr;
    if (!guild_player)
        throw 2;

    auto *vclient = guild_player->get_voice_client ();
    if (!vclient)
        throw 2;

    const std::string &fname = track.filename;

    std::chrono::high_resolution_clock::time_point start_time;

    const std::string music_folder_path = get_music_folder_path ();
    const std::string file_path = music_folder_path + fname;

    if (vclient && !vclient->terminating && vclient->is_ready ())
        {
            bool debug = get_debug_state ();

            guild_player->tried_continuing = false;

            FILE *ofile = fopen (file_path.c_str (), "r");

            if (!ofile)
                {
                    std::filesystem::create_directory (music_folder_path);
                    throw 2;
                }

            struct stat ofile_stat;
            if (fstat (fileno (ofile), &ofile_stat) != 0)
                {
                    fclose (ofile);
                    ofile = NULL;
                    throw 2;
                }

            fclose (ofile);
            ofile = NULL;

            track.filesize = ofile_stat.st_size;

            const std::string server_id_str = std::to_string (guild_id);
            const std::string slave_id = "processor-" + server_id_str + "."
                                         + std::to_string (time (NULL));

            std::string cmd
                = cc::command_options_keys_t.id + '=' + slave_id + ';'
                  + cc::command_options_keys_t.guild_id + '=' + server_id_str

                  + ';' + cc::command_options_keys_t.command + '='
                  + cc::command_execute_commands_t.create_audio_processor
                  + ';';

            if (debug)
                {
                    cmd += cc::command_options_keys_t.debug + "=1;";
                }

            cmd += cc::command_options_keys_t.file_path + '='
                   + cc::sanitize_command_value (file_path) + ';'

                   + cc::command_options_keys_t.volume + '='
                   + std::to_string (guild_player->volume) + ';';

            if (guild_player->fx_is_tempo_active ())
                cmd += cc::command_options_keys_t.helper_chain + '='
                       + cc::sanitize_command_value (
                           "!atempo=" + std::to_string (guild_player->tempo))
                       + ';';

            if (guild_player->fx_is_pitch_active ())
                cmd += cc::command_options_keys_t.helper_chain + '='
                       + cc::sanitize_command_value (
                           get_ffmpeg_pitch_args (guild_player->pitch))
                       + ';';

            if (guild_player->fx_is_equalizer_active ())
                cmd += cc::command_options_keys_t.helper_chain + '='
                       + cc::sanitize_command_value ("superequalizer="
                                                     + guild_player->equalizer)
                       + ';';

            if (guild_player->fx_is_sampling_rate_active ())
                cmd += cc::command_options_keys_t.helper_chain + '='
                       + cc::sanitize_command_value (
                           "aresample="
                           + std::to_string (guild_player->sampling_rate))
                       + ';';

            if (guild_player->fx_is_vibrato_active ())
                {
                    std::string v_args = get_ffmpeg_vibrato_args (
                        guild_player->fx_has_vibrato_f (),
                        guild_player->fx_has_vibrato_d (), guild_player);

                    cmd += cc::command_options_keys_t.helper_chain + '='
                           + cc::sanitize_command_value ("vibrato=" + v_args)
                           + ';';
                }

            if (guild_player->fx_is_tremolo_active ())
                {
                    std::string v_args = get_ffmpeg_tremolo_args (
                        guild_player->fx_has_tremolo_f (),
                        guild_player->fx_has_tremolo_d (), guild_player);

                    cmd += cc::command_options_keys_t.helper_chain + '='
                           + cc::sanitize_command_value ("tremolo=" + v_args)
                           + ';';
                }

            if (guild_player->fx_is_earwax_active ())
                cmd += cc::command_options_keys_t.helper_chain + '='
                       + cc::sanitize_command_value ("earwax") + ';';

            track.check_for_seek_to ();

            if (!track.seek_to.empty ())
                {
                    cmd += cc::command_options_keys_t.seek + '='
                           + cc::sanitize_command_value (track.seek_to) + ';';

                    track.seek_to = "";
                    guild_player->reset_first_track_current_byte ();
                }

            // const std::string exit_cmd = cc::get_exit_command (slave_id);
            // cc::send_command (cmd);
            // int status = cc::wait_slave_ready (slave_id, 10);

            // if (status != 0)
            // // !TODO: what to do here? shutting down existing processor is
            // // not right
            // throw 3;

            const std::string exit_cmd = cc::get_exit_command (slave_id);
            // kill when fail
            if (cc::send_command_wr (cmd, exit_cmd, slave_id, 10) != 0)
                throw 3;

            const std::string fifo_stream_path
                = audio_processing::get_audio_stream_fifo_path (slave_id);

            const std::string fifo_command_path
                = audio_processing::get_audio_stream_stdin_path (slave_id);

            const std::string fifo_notify_path
                = audio_processing::get_audio_stream_stdout_path (slave_id);

            // OPEN FIFOS
            int read_fd = open (fifo_stream_path.c_str (), O_RDONLY);
            if (read_fd < 0)
                {
                    cc::send_command (exit_cmd);
                    throw 2;
                }

            int command_fd = open (fifo_command_path.c_str (), O_WRONLY);
            if (command_fd < 0)
                {
                    cc::send_command (exit_cmd);
                    close (read_fd);
                    throw 2;
                }

            int notification_fd = open (fifo_notify_path.c_str (), O_RDONLY);
            if (notification_fd < 0)
                {
                    cc::send_command (exit_cmd);
                    close (read_fd);
                    close (command_fd);
                    throw 2;
                }

            // wait for processor notification
            char nbuf[CMD_BUFSIZE + 1];
            size_t nread_size = read (notification_fd, nbuf, CMD_BUFSIZE);

            bool processor_read_ready = false;
            if (nread_size > 0)
                {
                    nbuf[nread_size] = '\0';

                    if (std::string (nbuf) == "0")
                        {
                            processor_read_ready = true;
                        }
                }

            handle_effect_chain_change_states_t effect_states
                = { guild_player, track, command_fd,
                    read_fd,      NULL,  notification_fd };

            int throw_error = 0;
            bool running_state, is_stopping;

            if (!processor_read_ready)
                {
                    fprintf (stderr, msprrfmt, slave_id.c_str ());
                    cc::send_command (exit_cmd);
                    close (read_fd);
                    close (command_fd);
                    close (notification_fd);
                    throw 2;
                }

            EffectStatesListing esl (guild_id, &effect_states);

            float dpp_audio_buffer_length_second = get_stream_buffer_size ();
            int64_t dpp_audio_sleep_on_buffer_threshold_ms
                = get_stream_sleep_on_buffer_threshold_ms ();

            // I LOVE C++!!!

            // track.seekable = true;

            // using raw pcm need to change ffmpeg output format to s16le!
            ssize_t read_size = 0;
            ssize_t current_read = 0;
            ssize_t total_read = 0;
            uint8_t buffer[STREAM_BUFSIZ];

            while ((running_state = get_running_state ())
                   && !(is_stopping = guild_player->stopping)
                   && ((current_read = read (read_fd, buffer + read_size,
                                             STREAM_BUFSIZ - read_size))
                       > 0))
                {
                    read_size += current_read;
                    total_read += current_read;

                    wait_for_ready_event (guild_id);
                    if ((is_stopping = guild_player->stopping))
                        // !TODO: send shutdown command instead of breaking and
                        // abruptly closing output fd?
                        break;

                    if (read_size != STREAM_BUFSIZ)
                        continue;

                    // if ((debug = get_debug_state ()))
                    //     fprintf (stderr, "Sending buffer: %ld %ld\n",
                    //              total_read, read_size);

                    vclient = guild_player->get_voice_client ();
                    if (audio_processing::send_audio_routine (
                            vclient, (uint16_t *)buffer, &read_size, false,
                            guild_player->opus_encoder))
                        break;

                    handle_effect_chain_change (effect_states);

                    float outbuf_duration;

                    while (
                        (running_state = get_running_state ())
                        && !wait_for_ready_event (guild_id)
                        && (vclient = guild_player->get_voice_client ())
                        && vclient && !vclient->terminating
                        && ((outbuf_duration = vclient->get_secs_remaining ())
                            > dpp_audio_buffer_length_second))
                        {
                            handle_effect_chain_change (effect_states);

                            std::this_thread::sleep_for (
                                std::chrono::milliseconds (
                                    dpp_audio_sleep_on_buffer_threshold_ms));
                        }
                }

            if ((read_size > 0) && running_state && !is_stopping)
                {
                    if (debug)
                        fprintf (stderr, "Final buffer: %ld %ld\n",
                                 (total_read += read_size), read_size);

                    vclient = guild_player->get_voice_client ();
                    audio_processing::send_audio_routine (
                        vclient, (uint16_t *)buffer, &read_size, true,
                        guild_player->opus_encoder);
                }

            close (read_fd);
            close (command_fd);
            command_fd = -1;
            close (notification_fd);
            notification_fd = -1;

            if (debug)
                std::cerr << "Exiting " << guild_id << '\n';

            cc::send_command (exit_cmd);

            if (!running_state || is_stopping)
                {
                    // clear voice client buffer
                    vclient = guild_player->get_voice_client ();
                    if (vclient)
                        vclient->stop_audio ();
                }

            auto end_time = std::chrono::high_resolution_clock::now ();
            auto done = std::chrono::duration_cast<std::chrono::milliseconds> (
                end_time - start_time);

            if (debug)
                {
                    fprintf (stderr, "Done streaming for %lld milliseconds\n",
                             done.count ());
                    // fprintf (stderr, "audio_processing status: %d\n",
                    // status);
                }

            if (throw_error != 0)
                {
                    throw throw_error;
                }
        }
    else
        throw 1;
}

void
Manager::set_processor_state (std::string &server_id_str,
                              processor_state_t state)
{
    std::lock_guard lk (this->as_m);

    this->processor_states[server_id_str] = state;
}

processor_state_t
Manager::get_processor_state (std::string &server_id_str)
{
    std::lock_guard lk (this->as_m);
    auto i = this->processor_states.find (server_id_str);

    if (i == this->processor_states.end ())
        {
            return PROCESSOR_NULL;
        }

    return i->second;
}

bool
Manager::is_processor_ready (std::string &server_id_str)
{
    return get_processor_state (server_id_str) & PROCESSOR_READY;
}

bool
Manager::is_processor_dead (std::string &server_id_str)
{
    return get_processor_state (server_id_str) & PROCESSOR_DEAD;
}

} // musicat::player
