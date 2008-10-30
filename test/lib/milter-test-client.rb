#!/usr/bin/env ruby

require 'milter'
require 'socket'
require 'timeout'
require 'optparse'

class MilterTestClient
  def initialize
    @encoder = Milter::Encoder.new
    @decoder = Milter::CommandDecoder.new
    @state = :start
    @helo_fqdn = nil
    @mail_from = nil
    @rcpt_to = nil
    @headers = []
    @content = ""
    @socket = nil
    initialize_options
    setup_decoder
  end

  def run
    TCPServer.open(@port) do |socket|
      print_status("ready")

      Timeout.timeout(@timeout) do
        @socket = socket.accept
      end

      while packet = read_packet
        @decoder.decode(packet)
        break if [:quit, :shutdown].include?(@state)
      end
    end
  ensure
    @socket = nil
  end

  def parse_options(argv)
    opts = OptionParser.new do |opts|
      opts.on("--port=PORT", Integer, "Listen on PORT (#{@port})") do |port|
        @port = port
      end

      opts.on("--[no-]print-status",
              "Print message on status change") do |boolean|
        @print_status = boolean
      end

      opts.on("--timeout=TIMEOUT", Float,
              "Use TIMEOUT as timeout value") do |timeout|
        @timeout = timeout
      end

      opts.on("--[no-]debug", "Output debug information") do |boolean|
        @debug = boolean
      end

      opts.on("--action=ACTION",
              "Do ACTION when condition is matched",
              "(#{@current_action})") do |action|
        @current_action = action # FIXME: validate
      end

      opts.on("--envelope-receipt=RECEIPT",
              "Add RECEIPT targets to be applied ACTION") do |receipt|
        @receipts << [@current_action, receipt]
      end
    end
    opts.parse!(argv)
  end

  private
  def initialize_options
    @port = 9999
    @print_ready = false
    @timeout = 3
    @debug = false
    @current_action = "reject"
    @receipts = []
  end

  def print_status(status)
    return unless @print_status
    puts status
    $stdout.flush
  end

  def read_packet
    packet = nil
    Timeout.timeout(@timeout) do
      packet = @socket.readpartial(4096)
    end
    packet
  end

  def write(next_state, encode_type, *args)
    packet, packed_size = @encoder.send("encode_#{encode_type}", *args)
    while packet
      written_size = @socket.write(packet)
      packet = packet[written_size, -1]
    end
    info("#{@state} -> #{next_state}")
    @state = next_state
    packed_size
  end

  def info(*args)
    args.each {|arg| $stderr.puts(arg.inspect)} if @debug
  end

  def setup_decoder
    @decoder.class.signals.each do |signal|
      next if signal == "decode-command"
      @decoder.signal_connect(signal) do |_, *args|
        info(signal)
        status = "receive: #{signal}"
        normalized_signal = signal.gsub(/-/, '_')
        additional_info_callback_name = "info_#{normalized_signal}"
        if respond_to?(additional_info_callback_name, true)
          additional_info = send(additional_info_callback_name, *args)
          status << ": #{additional_info}" unless additional_info.to_s.empty?
        end
        print_status(status)
        callback_name = "do_#{normalized_signal}"
        send(callback_name, *args) if respond_to?(callback_name, true)
      end
    end
  end

  def do_negotiate(option)
    invalid_state(:negotiate) if @state != :start
    @option = option

    write(:negotiated, :reply_negotiate, option)
  end

  def do_connect(host, address)
    invalid_state(:connect) if @state != :negotiated
    @connect_host = host
    @connect_address = address

    write(:connected, :reply_continue)
  end

  def info_connect(host, address)
    [host, address.to_s].join(" ")
  end

  def do_helo(fqdn)
    invalid_state(:helo) if @state != :connected
    @hello_fqdn = fqdn

    write(:greeted, :reply_continue)
  end

  def do_mail(to)
    invalid_state(:mail) if @state != :greeted
    @mail_to = to

    write(:mailed, :reply_continue)
  end

  def do_rcpt(from)
    invalid_state(:rcpt) unless [:mailed, :receipted].include?(@state)
    @receipted_from = from

    @receipts.each do |action, receipt|
      if receipt == from
        write(action, "reply_#{action}")
        return
      end
    end
    write(:receipted, :reply_continue)
  end

  def do_data
    invalid_state(:data) if @state != :receipted

    write(:data, :reply_continue)
  end

  def do_header(name, value)
    invalid_state(:header) unless [:data, :header].include?(@state)
    @headers << [name, value]

    write(:header, :reply_continue)
  end

  def info_header(name, value)
    [name, value].join(" ")
  end

  def do_end_of_header
    invalid_state(:end_of_header) unless [:data, :header].include?(@state)

    write(:end_of_header, :reply_continue)
  end

  def do_body(chunk)
    invalid_state(:body) unless [:end_of_header, :body].include?(@state)

    @content << chunk

    write(:body, :reply_continue)
  end

  def info_body(chunk)
    chunk
  end

  def do_end_of_message(chunk)
    unless [:end_of_header, :body].include?(@state)
      invalid_state(:end_of_message)
    end

    write(:end_of_message, :reply_continue)
  end

  def info_end_of_message(chunk)
    chunk
  end

  def do_unknown(command)
    write(:unknown, :reply_reject)
  end

  def info_unknown(command)
    command
  end

  def do_abort
    @state = :abort
  end

  def do_quit
    @state = :quit
  end

  def invalid_state(reply_state)
    state = @state
    write(:shutdown, :reply_shutdown)
    raise "should not receive reply for #{reply_state} on #{state}"
  end
end

client = MilterTestClient.new
client.parse_options(ARGV)
client.run
