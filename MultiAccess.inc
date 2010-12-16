<?php

class MultiAccess
{
	public $read_q_key = 1;
	public $write_q_key = 2;
	public $writers_mutex_key = 3;
	public $err_log = array();
	public $log = array();
	private $read_q;
	private $write_q;
	private $writers_mutex;
	private $writers_count = 0;
	private $readers_count = 0;
	const readers = 1;
	const writers = 2;
	const wait_time = 1;
	const max_readers_queue = 100;
	const max_writers_queue = 100;

	public function get_access_read(&$auto_unlocker_reference = -1)
	{
		if (($this->writers_count <= 0) && ($this->readers_count <= 0)) //if it is nested call - access should be given only once
		{
			$writers = $this->get_counter(self::writers); //if somebody are writing here now - we will wait
			if ($writers > 0)
			{
				$readers = $this->get_counter(self::readers);
				if ($readers < self::max_readers_queue) $this->wait(self::writers); //if some writer become freezed - don't wait too long
			}
			$sent = $this->increment(); //increment count of readers - writers will be waiting for us while we read.
			if ($sent===false)
			{
				$this->err_log[] = "gar: not incremented";
				return false;
			}
		}

		if ($auto_unlocker_reference!=-1)
		{
			$mutex = new autoUnlocker(array($this, 'release_access_read'));
			$auto_unlocker_reference = $mutex;
		}
		$this->readers_count++;
		return true;
	}

	public function release_access_read($autoUnlocker = NULL)
	{
		if (!empty($autoUnlocker)) $autoUnlocker->revoke();
		if ($this->readers_count > 0) $this->readers_count--;
		if ($this->readers_count==0) $this->decrement(self::readers); //tell to writers, that we have read and they can write now
		return true;
	}

	public function get_access_write(&$auto_unlocker_reference = -1)
	{
		if ($this->writers_count <= 0) //if we are writers already - don't need to lock semaphore again
		{
			if ($this->readers_count > 0) //if we got reader's access and want to write - release our reader's access and got new access - writer's (else process will wait itself for a while)
			{
				while ($this->readers_count > 0) $this->release_access_read($auto_unlocker_reference);
			}
			//acquire mutex for writing
			if (empty($this->writers_mutex)) $this->writers_mutex = sem_get($this->writers_mutex_key, 1, 0777, 1);
			if (empty($this->writers_mutex)) return false;
			ignore_user_abort(true); //don't hang with semaphore, please :)
			set_time_limit(3);
			$starttime = microtime(true);
			if (!sem_acquire($this->writers_mutex)) return false;
			$acquire_time = microtime(true)-$starttime;
			if ($acquire_time >= 1) $this->err_log[] = 'Acquire time: '.round($acquire_time, 4);
			//tell to readers, that they should wait while we will write
			$sent = $this->increment(self::writers);
			if ($sent===false)
			{
				$this->err_log[] = "gaw: not incremented";
				return false;
			}
			//but if readers has come before us - wait, until they finish
			$readers = $this->get_counter(self::readers);
			if ($readers > 0 && $this->readers_count < $readers) //if data locked by readers, and not by ours
			{
				$writers = $this->get_counter(self::writers);
				if ($writers < self::max_writers_queue) $this->wait(self::readers); //if some reader become freezed - don't wait too long
			}
		}

		if ($auto_unlocker_reference!=-1)
		{
			$mutex = new autoUnlocker(array($this, 'release_access_write'));
			$auto_unlocker_reference = $mutex;
		}
		$this->writers_count++;
		//and now we can write :)
		return true;
	}

	public function release_access_write($autoUnlocker = NULL)
	{
		if (!empty($autoUnlocker)) $autoUnlocker->revoke();
		if ($this->writers_count > 0) $this->writers_count--;
		if ($this->writers_count==0)
		{
			$this->decrement(self::writers); //tell to readers, that they can read now
			if (empty($this->writers_mutex)) $this->writers_mutex = sem_get($this->writers_mutex_key, 1, 0777, 1);
			if (sem_release($this->writers_mutex)) unset($this->writers_mutex);
		}
		return true;
	}

	private function increment($type = self::readers)
	{
		$q = $this->select_q($type);
		if (empty($q)) return false;
		$sent = msg_send($q, $type, $type, false, false, $err);
		if ($sent==false) return false;
		return true;
	}

	private function decrement($type = self::readers)
	{
		$q = $this->select_q($type);
		if (empty($q)) return false;
		$recieve = msg_receive($q, $type, $t, 100, $msg, false, MSG_IPC_NOWAIT+MSG_NOERROR, $err);
		if ($recieve===false) return false;
		return true;
	}

	public function get_counter($type = self::writers)
	{
		$q = $this->select_q($type);
		$stat = msg_stat_queue($q);
		return $stat['msg_qnum'];
	}

	private function wait($type = self::writers)
	{
		$q = $this->select_q($type);
		if (empty($q)) return false;

		$starttime = time();
		do
		{
			$stat = msg_stat_queue($q);
			if ((time()-$starttime) > self::wait_time)
			{
				$this->err_log[] = 'waiting for '.$type."\n".$this->printable_backtrace();
				for ($i = 0; $i < $stat['msg_qnum']; $i++) $this->decrement($type);
				break;
			}
		}
		while ($stat['msg_qnum'] > 0);
		return true;
	}

	private function select_q($type)
	{
		if ($type==self::readers)
		{
			if (empty($this->read_q)) $this->read_q = msg_get_queue($this->read_q_key, 0777);
			$q = $this->read_q;
		}
		else
		{
			if (empty($this->write_q)) $this->write_q = msg_get_queue($this->write_q_key, 0777);
			$q = $this->write_q;
		}
		return $q;
	}

	public function __construct($id = '')
	{
		if (!empty($id))
		{
			if (is_string($id))
			{
				if (is_file($id) || is_dir($id)) $id = ftok($id, 'I');
				else  $id = intval($id);
			}
			$this->read_q_key += $id*10;
			$this->write_q_key += $id*10;
			$this->writers_mutex_key += $id*10;
		}
	}

	public function __destruct()
	{
		if ($this->writers_count > 0)
		{
			$this->err_log[] = 'writers count = '.$this->writers_count;
			while ($this->writers_count > 0) $this->release_access_write();
		}
		elseif (!empty($this->writers_mutex)) @sem_release($this->writers_mutex);
		if ($this->readers_count > 0)
		{
			$this->err_log[] = 'readers count = '.$this->readers_count;
			while ($this->readers_count > 0) $this->release_access_read();
		}

		if (!empty($this->err_log))
		{
			trigger_error('MultiAccess errors '.implode("\n", $this->err_log), E_USER_WARNING);
		}
	}

	private static function printable_backtrace()
	{
		$tmp = debug_backtrace();
		array_shift($tmp);
		$arr = array_reverse($tmp);
		$str = '';
		$space = $basespace = '|';
		foreach ($arr as $t)
		{
			$str .= ' '.$space.$t['file']."\t[".$t['line']."]\t";
			if (isset($t['class'])) $str .= $t['class'].$t['type'];
			$str .= $t['function'];
			if (isset($t['args'][0]))
			{
				$str .= '("';
				foreach ($t['args'] as $t_arg)
				{
					if (!is_scalar($t_arg)) $str .= '[scalar],';
					else  $str .= $t_arg.',';
				}
				$str .= '")';
			}
			else  $str .= '()';
			$str .= "\n";
			$space .= $basespace;
		}
		return rtrim($str);
	}
}

/** thanks to authors of RAII! */
class autoUnlocker
{
	protected $Unlock;

	/**
	 * Register function to auto-unlock mutex
	 * @param callback $Unlock
	 */
	public function __construct($Unlock)
	{
		if (is_callable($Unlock)) $this->Unlock = $Unlock;
	}

	/** Revoke auto-unlocking (if mutex unlocked was "manually") */
	public function revoke()
	{
		unset($this->Unlock);
	}

	/** Unlock mutex automatically */
	public function __destruct()
	{
		if (isset($this->Unlock)) call_user_func($this->Unlock, $this);
	}

}