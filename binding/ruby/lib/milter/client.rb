# Copyright (C) 2009-2010  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this library.  If not, see <http://www.gnu.org/licenses/>.

module Milter
  class Client
    def register(session_class)
      signal_connect("connection-established") do |_client, context|
        setup_session(context, session_class)
      end
    end

    private
    def setup_session(context, session_class)
      session = session_class.new
      context.instance_variable_set("@session", session)

      [:negotiate, :connect, :helo, :envelope_from, :envelope_recipient,
       :data, :unknown, :header, :end_of_header, :body, :end_of_message,
       :abort].each do |event|
        if session.respond_to?(event)
          context.signal_connect(event) do |_context, *args|
            session_context = ClientSessionContext.new(_context)
            session.send(event, session_context, *args)
            session_context.status
          end
        end
      end
    end
  end

  class ClientSession
    def negotiate(context, option, macros_requests)
      [:connect, :helo, :envelope_from, :envelope_recipient,
       :body, :unknown, [:header, :headers], :end_of_header,
      ].each do |method_name, step_name|
        if respond_to?(method_name)
          step_name ||= method_name
          step = Milter::Step.const_get("NO_#{step_name.to_s.upcase}")
          option.remove_action(step)
        end
      end
      context.status = :continue
    end
  end

  class ClientSessionContext
    def initialize(context)
      @context = context
      @status = nil
    end

    def status=(value)
      case value
      when String, Symbol
        unless Milter::Status.const_defined?(value.to_s.upcase)
          raise ArgumentError, "unknown status: <#{value.inspect}>"
        end
      when Milter::Status, nil
      else
        message =
          "should be one of [String, Symbol, Milter::Status, nil]: " +
          "<#{value.inspect}>"
        raise ArgumentError, message
      end
      @status = value
    end

    def status
      status = @status || Milter::Status::DEFAULT
      if status.is_a?(String) or status.is_a?(Symbol)
        status = Milter::Status.const_get(status.to_s.upcase)
      end
      status
    end

    def set_reply(code, extended_code, reason)
      @context.set_reply(code, extended_code, reason)
      if 400 <= code and code < 500
        self.status = :temporary_failure
      elsif 500 <= code and code < 600
        self.status = :reject
      end
    end

    def method_missing(*args, &block)
      @context.send(*args, &block)
    end
  end
end
